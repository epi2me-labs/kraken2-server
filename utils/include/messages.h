#include "Kraken2.grpc.pb.h"
#include "seqreader.h"

bool SequenceRequestToSequence(kraken2proto::Kraken2SequenceRequest &req, kraken2::Sequence &seq);
