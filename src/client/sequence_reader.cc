#include "sequence_reader.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include <zlib.h>

#include "kseq.h"

KSEQ_INIT(gzFile, gzread)

using kraken2proto::Kraken2SequenceRequest;

struct SequenceReader::Impl
{
    std::string path;
    gzFile file = nullptr;
    kseq_t *seq = nullptr;

    ~Impl()
    {
        if (seq)
            kseq_destroy(seq);
        if (file)
            gzclose(file);
    }
};

SequenceReader::SequenceReader(const std::string &path) : impl_(new Impl)
{
    impl_->path = path;
    // gzopen/gzdopen read plain text transparently as well as gzip. For stdin
    // we dup, so closing our gzFile doesn't close the process's own stdin.
    if (path == "-") {
        int fd = dup(fileno(stdin));
        impl_->file = fd < 0 ? nullptr : gzdopen(fd, "r");
    }
    else {
        impl_->file = gzopen(path.c_str(), "r");
    }
    if (impl_->file == nullptr)
        throw std::runtime_error("unable to open " + path + ": " + strerror(errno));
    impl_->seq = kseq_init(impl_->file);
}

SequenceReader::~SequenceReader() = default;

size_t SequenceReader::ReadBatch(std::vector<Kraken2SequenceRequest> &batch, size_t max_reads)
{
    batch.clear();
    batch.reserve(max_reads);
    kseq_t *seq = impl_->seq;
    while (batch.size() < max_reads) {
        int status = kseq_read(seq);
        if (status == -1)   // end of input
            break;
        if (status == -2)
            throw std::runtime_error(
                "malformed record (sequence and quality lengths differ) after read '" +
                std::string(seq->name.s ? seq->name.s : "") + "'");
        if (status < -1)
            throw std::runtime_error("error reading " + impl_->path);

        const bool fastq = seq->qual.l != 0;
        std::string header(fastq ? "@" : ">");
        header.append(seq->name.s);
        if (seq->comment.l > 0) {
            header.append(" ");
            header.append(seq->comment.s);
        }

        batch.emplace_back();
        Kraken2SequenceRequest &request = batch.back();
        request.set_id(seq->name.s);
        request.set_seq(seq->seq.s);
        request.set_header(header);
        request.set_format(fastq ? Kraken2SequenceRequest::FORMAT_FASTQ
                                 : Kraken2SequenceRequest::FORMAT_FASTA);
        request.set_quals(fastq ? seq->qual.s : "");
    }
    return batch.size();
}
