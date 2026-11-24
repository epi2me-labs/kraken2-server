#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "kseq.cc.h"


FastReader::FastReader(std::string filename)
{
    m_filename = filename;
    // "-" is stdin. gzopen/gzdopen read plain text transparently as well as gzip.
    // (dup so that closing our gzFile doesn't close the process's stdin.)
    if (filename == "-") {
        int fd = dup(fileno(stdin));
        m_fp = fd < 0 ? nullptr : gzdopen(fd, "r");
    }
    else {
        m_fp = gzopen(filename.c_str(), "r");
    }
    if (m_fp == nullptr) {
        throw std::runtime_error("unable to open " + filename + ": " + strerror(errno));
    }
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
        // -1 is end of file, anything else is a malformed record or read error
        if (rtn == -2) {
            throw std::runtime_error("malformed record (sequence and quality lengths differ) after read '" +
                                     std::string(m_seq->name.s ? m_seq->name.s : "") + "'");
        }
        if (rtn < -1) {
            throw std::runtime_error("error reading " + m_filename);
        }
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

        rec.set_id(m_seq->name.s);
        rec.set_seq(m_seq->seq.s);
        rec.set_header(header);
        if (m_seq->qual.l == 0)
        {
            rec.set_format(Kraken2SequenceRequest::FORMAT_FASTA);
            rec.set_quals("");
        }
        else
        {
            rec.set_format(Kraken2SequenceRequest::FORMAT_FASTQ);
            rec.set_quals(m_seq->qual.s);
        }
    }
    return rtn;
}


int FastReader::read(std::vector<Kraken2SequenceRequest> &seqs, int batch_size)
{
    int rtn = 0;
    seqs.reserve(batch_size);
    for(int i=0; i<batch_size; ++i) {
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