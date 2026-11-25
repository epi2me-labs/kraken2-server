#pragma once

#include <memory>
#include <string>

#include "compact_hash.h"
#include "kraken2_data.h"
#include "kv_store.h"
#include "taxonomy.h"

namespace k2s {

/**
 * A kraken2 database, loaded once and then only read, so it can be shared by
 * any number of classifying threads.
 */
class Index
{
public:
    /**
     * Load the database in `db_dir` (taxo.k2d, opts.k2d and hash.k2d).
     * @param memory_mapping map the files rather than reading them into memory
     * @throws std::runtime_error if a file is missing or not a valid database
     */
    Index(const std::string &db_dir, bool memory_mapping);

    const kraken2::IndexOptions &options() const { return options_; }
    const kraken2::KeyValueStore &hash() const { return *hash_; }

    /// Whether reads are searched by translating them into six reading frames
    /// (the database was built from proteins).
    bool translated() const { return !options_.dna_db; }

    /**
     * Not const-qualified in the type, because kraken2's report functions take
     * a mutable reference even though they only read. Nothing here modifies it.
     */
    kraken2::Taxonomy &taxonomy() const { return *taxonomy_; }

private:
    kraken2::IndexOptions options_ = {};
    std::unique_ptr<kraken2::Taxonomy> taxonomy_;
    // 32- or 40-bit cell table, which one depends on the database
    std::unique_ptr<kraken2::KeyValueStore> hash_;
};

}  // namespace k2s
