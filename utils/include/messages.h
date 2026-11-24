#pragma once
#include "Kraken2.grpc.pb.h"
#include "sequence.h"

bool SequenceRequestToSequence(
    const kraken2proto::Kraken2SequenceRequest &req, k2server::Sequence &seq);
