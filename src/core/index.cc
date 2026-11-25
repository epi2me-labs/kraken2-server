#include "core/index.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>

#include "aa_translate.h"

namespace k2s {

namespace {

void RequireReadable(const std::string &filename)
{
    if (access(filename.c_str(), R_OK) != 0)
        throw std::runtime_error("cannot read " + filename + ": " + strerror(errno));
}

kraken2::IndexOptions ReadOptions(const std::string &filename)
{
    struct stat sb;
    if (stat(filename.c_str(), &sb) < 0)
        throw std::runtime_error("unable to get size of " + filename);
    if (static_cast<size_t>(sb.st_size) != sizeof(kraken2::IndexOptions))
        throw std::runtime_error(
            filename + " is not the size expected of a database options file");
    kraken2::IndexOptions options = {};
    std::ifstream in(filename, std::ios::binary);
    if (!in.read(reinterpret_cast<char *>(&options), sizeof(options)))
        throw std::runtime_error("unable to read " + filename);
    return options;
}

}  // namespace

Index::Index(const std::string &db_dir, bool memory_mapping)
{
    const std::string taxonomy_file = db_dir + "/taxo.k2d";
    const std::string options_file = db_dir + "/opts.k2d";
    const std::string hash_file = db_dir + "/hash.k2d";
    RequireReadable(options_file);
    RequireReadable(taxonomy_file);
    RequireReadable(hash_file);

    options_ = ReadOptions(options_file);

    // Upstream's translation tables are empty until this is called (kraken2's
    // classify calls it at the start of a run). Without it every codon
    // translates to 'K' and a protein database classifies nothing.
    static std::once_flag tables_ready;
    std::call_once(tables_ready, [] { kraken2::initLookUpTables(); });

    taxonomy_.reset(new kraken2::Taxonomy(taxonomy_file, memory_mapping));

    // Upstream databases come with either 32 or 40 bit cells.
    std::string hash_file_name = hash_file;
    switch (kraken2::GetKVStoreCellType(hash_file_name)) {
    case kraken2::CompactHash32:
        hash_.reset(new kraken2::CompactHashTable<kraken2::CompactHashCell>(
            hash_file, memory_mapping));
        break;
    case kraken2::CompactHash40:
        hash_.reset(new kraken2::CompactHashTable<kraken2::CompactHashCell40>(
            hash_file, memory_mapping));
        break;
    default:
        throw std::runtime_error("unable to determine width of compact hash cell in " + hash_file);
    }
}

}  // namespace k2s
