#include <string>

#include "Kraken2.grpc.pb.h"

#include <zlib.h>
#include "kseq.h"

using kraken2proto::Kraken2SequenceRequest;

KSEQ_INIT(gzFile, gzread)

class FastReader
{
public:
    FastReader(std::string filename);
    ~FastReader();
    // Read (at most) one sequence
    int read(Kraken2SequenceRequest&);
    // read (up to) batch_size sequences
    int read(std::vector<Kraken2SequenceRequest> &seqs, int batch_size);
    // read all sequences
    int read_all(std::vector<Kraken2SequenceRequest> &seqs);
private:
    std::string m_filename;
    gzFile m_fp;
    kseq_t *m_seq;
};