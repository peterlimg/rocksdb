// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A database can be configured with a custom FilterPolicy object.
// This object is responsible for creating a small filter from a set
// of keys.  These filters are stored in rocksdb and are consulted
// automatically by rocksdb to decide whether or not to read some
// information from disk. In many cases, a filter can cut down the
// number of disk seeks form a handful to a single disk seek per
// DB::Get() call.
//
// Most people will want to use the builtin bloom filter support (see
// NewBloomFilterPolicy() below).

#pragma once

#include <stdlib.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "rocksdb/advanced_options.h"
#include "rocksdb/status.h"
#include "rocksdb/types.h"

namespace ROCKSDB_NAMESPACE {

class Slice;
struct BlockBasedTableOptions;
struct ConfigOptions;

// A class that takes a bunch of keys, then generates filter
class FilterBitsBuilder {
 public:
  virtual ~FilterBitsBuilder() {}

  // Add a key (or prefix) to the filter. Typically, a builder will keep
  // a set of 64-bit key hashes and only build the filter in Finish
  // when the final number of keys is known. Keys are added in sorted order
  // and duplicated keys are possible, so typically, the builder will
  // only add this key if its hash is different from the most recently
  // added.
  virtual void AddKey(const Slice& key) = 0;

  // Called by RocksDB before Finish to populate
  // TableProperties::num_filter_entries, so should represent the
  // number of unique keys (and/or prefixes) added, but does not have
  // to be exact.
  virtual size_t EstimateEntriesAdded() {
    // Default implementation for backward compatibility.
    // 0 conspicuously stands for "unknown".
    return 0;
  }

  // Generate the filter using the keys that are added
  // The return value of this function would be the filter bits,
  // The ownership of actual data is set to buf
  virtual Slice Finish(std::unique_ptr<const char[]>* buf) = 0;

  // Similar to Finish(std::unique_ptr<const char[]>* buf), except that
  // for a non-null status pointer argument, it will point to
  // Status::Corruption() when there is any corruption during filter
  // construction or Status::OK() otherwise.
  //
  // WARNING: do not use a filter resulted from a corrupted construction
  virtual Slice Finish(std::unique_ptr<const char[]>* buf,
                       Status* /* status */) {
    return Finish(buf);
  }

  // Verify the filter returned from calling FilterBitsBuilder::Finish.
  // The function returns Status::Corruption() if there is any corruption in the
  // constructed filter or Status::OK() otherwise.
  //
  // Implementations should normally consult
  // FilterBuildingContext::table_options.detect_filter_construct_corruption
  // to determine whether to perform verification or to skip by returning
  // Status::OK(). The decision is left to the FilterBitsBuilder so that
  // verification prerequisites before PostVerify can be skipped when not
  // configured.
  //
  // RocksDB internal will always call MaybePostVerify() on the filter after
  // it is returned from calling FilterBitsBuilder::Finish
  // except for FilterBitsBuilder::Finish resulting a corruption
  // status, which indicates the filter is already in a corrupted state and
  // there is no need to post-verify
  virtual Status MaybePostVerify(const Slice& /* filter_content */) {
    return Status::OK();
  }

  // Approximate the number of keys that can be added and generate a filter
  // <= the specified number of bytes. Callers (including RocksDB) should
  // only use this result for optimizing performance and not as a guarantee.
  // This default implementation is for compatibility with older custom
  // FilterBitsBuilders only implementing deprecated CalculateNumEntry.
  virtual size_t ApproximateNumEntries(size_t bytes) {
    bytes = std::min(bytes, size_t{0xffffffff});
    return static_cast<size_t>(CalculateNumEntry(static_cast<uint32_t>(bytes)));
  }

  // Old, DEPRECATED version of ApproximateNumEntries. This is not
  // called by RocksDB except as the default implementation of
  // ApproximateNumEntries for API compatibility.
  virtual int CalculateNumEntry(const uint32_t bytes) {
    // DEBUG: ideally should not rely on this implementation
    assert(false);
    // RELEASE: something reasonably conservative: 2 bytes per entry
    return static_cast<int>(bytes / 2);
  }
};

// A class that checks if a key can be in filter
// It should be initialized by Slice generated by BitsBuilder
class FilterBitsReader {
 public:
  virtual ~FilterBitsReader() {}

  // Check if the entry match the bits in filter
  virtual bool MayMatch(const Slice& entry) = 0;

  // Check if an array of entries match the bits in filter
  virtual void MayMatch(int num_keys, Slice** keys, bool* may_match) {
    for (int i = 0; i < num_keys; ++i) {
      may_match[i] = MayMatch(*keys[i]);
    }
  }
};

// Contextual information passed to BloomFilterPolicy at filter building time.
// Used in overriding FilterPolicy::GetBuilderWithContext(). References other
// structs because this is expected to be a temporary, stack-allocated object.
struct FilterBuildingContext {
  // This constructor is for internal use only and subject to change.
  FilterBuildingContext(const BlockBasedTableOptions& table_options);

  // Options for the table being built
  const BlockBasedTableOptions& table_options;

  // BEGIN from (DB|ColumnFamily)Options in effect at table creation time
  CompactionStyle compaction_style = kCompactionStyleLevel;

  // Number of LSM levels, or -1 if unknown
  int num_levels = -1;

  // An optional logger for reporting errors, warnings, etc.
  Logger* info_log = nullptr;
  // END from (DB|ColumnFamily)Options

  // Name of the column family for the table (or empty string if unknown)
  // TODO: consider changing to Slice
  std::string column_family_name;

  // The table level at time of constructing the SST file, or -1 if unknown
  // or N/A as in SstFileWriter. (The table file could later be used at a
  // different level.)
  int level_at_creation = -1;

  // True if known to be going into bottommost sorted run for applicable
  // key range (which might not even be last level with data). False
  // otherwise.
  bool is_bottommost = false;

  // Reason for creating the file with the filter
  TableFileCreationReason reason = TableFileCreationReason::kMisc;
};

// We add a new format of filter block called full filter block
// This new interface gives you more space of customization
//
// For the full filter block, you can plug in your version by implement
// the FilterBitsBuilder and FilterBitsReader
//
// There are two sets of interface in FilterPolicy
// Set 1: CreateFilter, KeyMayMatch: used for blockbased filter
// Set 2: GetFilterBitsBuilder, GetFilterBitsReader, they are used for
// full filter.
// Set 1 MUST be implemented correctly, Set 2 is optional
// RocksDB would first try using functions in Set 2. if they return nullptr,
// it would use Set 1 instead.
// You can choose filter type in NewBloomFilterPolicy
class FilterPolicy {
 public:
  virtual ~FilterPolicy();

  // Creates a new FilterPolicy based on the input value string and returns the
  // result The value might be an ID, and ID with properties, or an old-style
  // policy string.
  // The value describes the FilterPolicy being created.
  // For BloomFilters, value may be a ":"-delimited value of the form:
  //   "bloomfilter:[bits_per_key]:[use_block_based_builder]",
  //   e.g. ""bloomfilter:4:true"
  //   The above string is equivalent to calling NewBloomFilterPolicy(4, true).
  static Status CreateFromString(const ConfigOptions& config_options,
                                 const std::string& value,
                                 std::shared_ptr<const FilterPolicy>* result);

  // Return the name of this policy.  Note that if the filter encoding
  // changes in an incompatible way, the name returned by this method
  // must be changed.  Otherwise, old incompatible filters may be
  // passed to methods of this type.
  virtual const char* Name() const = 0;

  // DEPRECATED: This function is part of the deprecated block-based
  // filter, which will be removed in a future release.
  //
  // keys[0,n-1] contains a list of keys (potentially with duplicates)
  // that are ordered according to the user supplied comparator.
  // Append a filter that summarizes keys[0,n-1] to *dst.
  //
  // Warning: do not change the initial contents of *dst.  Instead,
  // append the newly constructed filter to *dst.
  virtual void CreateFilter(const Slice* keys, int n,
                            std::string* dst) const = 0;

  // DEPRECATED: This function is part of the deprecated block-based
  // filter, which will be removed in a future release.
  //
  // "filter" contains the data appended by a preceding call to
  // CreateFilter() on this class.  This method must return true if
  // the key was in the list of keys passed to CreateFilter().
  // This method may return true or false if the key was not on the
  // list, but it should aim to return false with a high probability.
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;

  // Return a new FilterBitsBuilder for full or partitioned filter blocks, or
  // nullptr if using block-based filter.
  // NOTE: This function is only called by GetBuilderWithContext() below for
  // custom FilterPolicy implementations. Thus, it is not necessary to
  // override this function if overriding GetBuilderWithContext().
  // DEPRECATED: This function will be removed in a future release.
  virtual FilterBitsBuilder* GetFilterBitsBuilder() const { return nullptr; }

  // A newer variant of GetFilterBitsBuilder that allows a FilterPolicy
  // to customize the builder for contextual constraints and hints.
  // (Name changed to avoid triggering -Werror=overloaded-virtual.)
  // If overriding GetFilterBitsBuilder() suffices, it is not necessary to
  // override this function.
  virtual FilterBitsBuilder* GetBuilderWithContext(
      const FilterBuildingContext&) const {
    return GetFilterBitsBuilder();
  }

  // Return a new FilterBitsReader for full or partitioned filter blocks, or
  // nullptr if using block-based filter.
  // As here, the input slice should NOT be deleted by FilterPolicy.
  virtual FilterBitsReader* GetFilterBitsReader(
      const Slice& /*contents*/) const {
    return nullptr;
  }
};

// Return a new filter policy that uses a bloom filter with approximately
// the specified number of bits per key.
//
// bits_per_key: average bits allocated per key in bloom filter. A good
// choice is 9.9, which yields a filter with ~ 1% false positive rate.
// When format_version < 5, the value will be rounded to the nearest
// integer. Recommend using no more than three decimal digits after the
// decimal point, as in 6.667.
//
// use_block_based_builder: use deprecated block based filter (true) rather
// than full or partitioned filter (false).
//
// Callers must delete the result after any database that is using the
// result has been closed.
//
// Note: if you are using a custom comparator that ignores some parts
// of the keys being compared, you must not use NewBloomFilterPolicy()
// and must provide your own FilterPolicy that also ignores the
// corresponding parts of the keys.  For example, if the comparator
// ignores trailing spaces, it would be incorrect to use a
// FilterPolicy (like NewBloomFilterPolicy) that does not ignore
// trailing spaces in keys.
extern const FilterPolicy* NewBloomFilterPolicy(
    double bits_per_key, bool use_block_based_builder = false);

// A new Bloom alternative that saves about 30% space compared to
// Bloom filters, with similar query times but roughly 3-4x CPU time
// and 3x temporary space usage during construction.  For example, if
// you pass in 10 for bloom_equivalent_bits_per_key, you'll get the same
// 0.95% FP rate as Bloom filter but only using about 7 bits per key.
//
// The space savings of Ribbon filters makes sense for lower (higher
// numbered; larger; longer-lived) levels of LSM, whereas the speed of
// Bloom filters make sense for highest levels of LSM. Setting
// bloom_before_level allows for this design with Level and Universal
// compaction styles. For example, bloom_before_level=1 means that Bloom
// filters will be used in level 0, including flushes, and Ribbon
// filters elsewhere, including FIFO compaction and external SST files.
// For this option, memtable flushes are considered level -1 (so that
// flushes can be distinguished from intra-L0 compaction).
// bloom_before_level=0 (default) -> Generate Bloom filters only for
// flushes under Level and Universal compaction styles.
// bloom_before_level=-1 -> Always generate Ribbon filters (except in
// some extreme or exceptional cases).
//
// Ribbon filters are compatible with RocksDB >= 6.15.0. Earlier
// versions reading the data will behave as if no filter was used
// (degraded performance until compaction rebuilds filters). All
// built-in FilterPolicies (Bloom or Ribbon) are able to read other
// kinds of built-in filters.
//
// Note: the current Ribbon filter schema uses some extra resources
// when constructing very large filters. For example, for 100 million
// keys in a single filter (one SST file without partitioned filters),
// 3GB of temporary, untracked memory is used, vs. 1GB for Bloom.
// However, the savings in filter space from just ~60 open SST files
// makes up for the additional temporary memory use.
//
// Also consider using optimize_filters_for_memory to save filter
// memory.
extern const FilterPolicy* NewRibbonFilterPolicy(
    double bloom_equivalent_bits_per_key, int bloom_before_level = 0);

// Old name and old default behavior (DEPRECATED)
inline const FilterPolicy* NewExperimentalRibbonFilterPolicy(
    double bloom_equivalent_bits_per_key) {
  return NewRibbonFilterPolicy(bloom_equivalent_bits_per_key, -1);
}

}  // namespace ROCKSDB_NAMESPACE
