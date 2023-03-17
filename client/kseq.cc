#include <string>

#include "kseq.cc.h"


FastReader::FastReader(std::string filename)
{
    m_filename = filename;
    gzFile m_fp = gzopen(filename.c_str(), "r");
    m_seq = kseq_init(m_fp);
}


FastReader::~FastReader()
{
    kseq_destroy(m_seq);
    gzclose(m_fp); 
}


int FastReader::read(Kraken2SequenceRequest& rec) {
    int rtn;
    if ((rtn = kseq_read(m_seq)) < 0) {
        rec.Clear();
        return rtn;
    }
    else {
        std::string header;
        m_seq->qual.l == 0 ? header.append(">") : header.append("@");
        header.append(m_seq->name.s);
        if(m_seq->comment.l > 0) {
            header.append(" ");
            header.append(m_seq->comment.s);
        }

        std::string str_rep;
        str_rep.append(header);
        str_rep.append("\n");

        rec.set_id(m_seq->name.s);
        rec.set_seq(m_seq->seq.s);
        rec.set_header(header);
        if (m_seq->qual.l == 0)
        {
            rec.set_format(Kraken2SequenceRequest::FORMAT_FASTA);
            rec.set_quals("");
            str_rep.append(rec.seq());
            str_rep.append("\n");
        }
        else
        {
            rec.set_format(Kraken2SequenceRequest::FORMAT_FASTQ);
            rec.set_quals(m_seq->qual.s);
            str_rep.append(rec.seq());
            str_rep.append("\n+\n");
            str_rep.append(rec.quals());
            str_rep.append("\n");
        }
        rec.set_str_representation(str_rep);
    }
    return rtn;
}


int FastReader::read(std::vector<Kraken2SequenceRequest> &seqs, int batch_size)
{
    int rtn = 0;
    seqs.reserve(batch_size);
    for(size_t i=0; i<batch_size; ++i) {
        Kraken2SequenceRequest rec;
        if (read(rec) >= 0) {
            rtn++;
            seqs.push_back(std::move(rec));
        }
        else {
            break;
        }
    }
    return rtn;
}


int FastReader::read_all(std::vector<Kraken2SequenceRequest> &seqs)
{
    int rtn = 0;
    while (true) {
        Kraken2SequenceRequest rec;
        if (read(rec) >= 0) {
            rtn++;
            seqs.push_back(std::move(rec));
        }
        else {
            break;
        }
    }
    return rtn;
}