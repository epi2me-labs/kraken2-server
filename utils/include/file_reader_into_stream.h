#pragma once

#include <cstdint>
#include <string>
#include "sys/errno.h"

#include "sequential_file_reader.h"
#include "messages.h"
#include "utils.h"

template <class StreamWriter>
class FileReaderIntoStream : public SequentialFileReader
{
public:
    FileReaderIntoStream(const std::string &filename, int file_num, StreamWriter &writer)
        : SequentialFileReader(filename), m_writer(writer), m_file_num(file_num)
    {
    }

    using SequentialFileReader::SequentialFileReader;
    using SequentialFileReader::operator=;

protected:
    virtual void OnChunkAvailable(const void *data, size_t size) override
    {
        const std::string remote_filename = extract_basename(GetFilePath());
        auto fc = MakeKraken2SequenceContent(m_file_num, data, size);
        if (!m_writer.Write(fc))
        {
            raise_from_system_error_code("The server aborted the connection.", ECONNRESET);
        }
    }

private:
    StreamWriter &m_writer;
    int m_file_num;
};
