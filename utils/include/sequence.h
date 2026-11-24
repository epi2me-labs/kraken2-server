#pragma once
// Upstream kraken2 removed its owning `Sequence` record (seqreader) in favour
// of the non-owning `SeqView`. The server receives reads as gRPC messages and
// needs an owning record, so it keeps its own.
#include <string>

#include "fast_reader.h"

namespace k2server {

struct Sequence {
    kraken2::SequenceFormat format = kraken2::FORMAT_AUTO_DETECT;
    std::string header;
    std::string id;
    std::string seq;
    std::string quals;
};

}  // namespace k2server
