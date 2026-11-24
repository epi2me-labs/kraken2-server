#include "messages.h"

bool SequenceRequestToSequence(
    const kraken2proto::Kraken2SequenceRequest &req, k2server::Sequence &seq)
{
    switch (req.format())
    {
    case kraken2proto::Kraken2SequenceRequest_SequenceFormat_FORMAT_AUTO_DETECT:
        seq.format = kraken2::SequenceFormat::FORMAT_AUTO_DETECT;
        break;
    case kraken2proto::Kraken2SequenceRequest_SequenceFormat_FORMAT_FASTQ:
        seq.format = kraken2::SequenceFormat::FORMAT_FASTQ;
        break;
    case kraken2proto::Kraken2SequenceRequest_SequenceFormat_FORMAT_FASTA:
        seq.format = kraken2::SequenceFormat::FORMAT_FASTA;
        break;
    }

    seq.header = req.header();
    seq.id = req.id();
    seq.seq = req.seq();
    seq.quals = req.quals();

    return true;
}