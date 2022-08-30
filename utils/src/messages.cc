#include "messages.h"

bool SequenceRequestToSequence(kraken2proto::Kraken2SequenceRequest &req, kraken2::Sequence &seq)
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

    seq.header = std::string(req.header());
    seq.id = std::string(req.id());
    seq.seq = std::string(req.seq());
    seq.quals = std::string(req.quals());

    return true;
}