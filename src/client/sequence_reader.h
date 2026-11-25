#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Kraken2.grpc.pb.h"

/**
 * Reads sequences from a FASTA or FASTQ file, plain or gzip compressed.
 * ("-" reads standard input.) The reading itself is kseq's.
 */
class SequenceReader
{
public:
    /// @throws std::runtime_error if the file cannot be opened
    explicit SequenceReader(const std::string &path);
    ~SequenceReader();

    SequenceReader(const SequenceReader &) = delete;
    SequenceReader &operator=(const SequenceReader &) = delete;

    /**
     * Read up to `max_reads` sequences, replacing the contents of `batch`.
     * @return how many were read; 0 once the input is exhausted
     * @throws std::runtime_error for a malformed record or a read error
     */
    size_t ReadBatch(std::vector<kraken2proto::Kraken2SequenceRequest> &batch, size_t max_reads);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
