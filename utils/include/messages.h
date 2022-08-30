#pragma once

#include <cstdint>
#include <string>

#include "Kraken2.grpc.pb.h"
#include "seqreader.h"

kraken2proto::Kraken2SequenceRequest MakeKraken2SequenceRequest(int file_num, const void *data, size_t data_len);
bool SequenceRequestToSequence(kraken2proto::Kraken2SequenceRequest &req, kraken2::Sequence &seq);
