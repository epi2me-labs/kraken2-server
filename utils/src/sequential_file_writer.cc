#include <iostream>
#include <utility>
#include <stdexcept>
#include <cstdio>
#include <sstream>
#include <sys/errno.h>

#include "utils.h"
#include "sequential_file_writer.h"

SequentialFileWriter::SequentialFileWriter() : m_no_space(false) {}
SequentialFileWriter::SequentialFileWriter(SequentialFileWriter &&) = default;
SequentialFileWriter &SequentialFileWriter::operator=(SequentialFileWriter &&) = default;

void SequentialFileWriter::Write(const std::string &name, std::string &data)
{
    // Open the file if it is not already open.
    OpenIfNecessary(name);
    try
    {
        // Write the data into the open file stream.
        m_ofs << data;
    }
    catch (const std::system_error &ex)
    {
        // On exception, close the file if open.
        if (m_ofs.is_open())
        {
            m_ofs.close();
        }
        // Make best effort to delete the file.
        std::remove(m_name.c_str());
        // Raise the error that occurred when writing to the open, now closed and deleted, file.
        RaiseError("writing to", ex);
    }
    // Erase the now written data.
    data.clear();
    return;
}

void SequentialFileWriter::OpenIfNecessary(const std::string &name)
{
    // Already open
    if (m_ofs.is_open())
    {
        return;
    }

    // Create an open file stream that can throw fail bit and bad bit exceptions - sets exception mask.
    std::ofstream ofs;
    ofs.exceptions(std::ifstream::failbit | std::ifstream::badbit);

    // Open the file configured to:
    // • be written to
    // • discard content within the file prior upon opening
    // • perform writing operations with raw binary
    try
    {
        ofs.open(name, std::ios_base::out | std::ios_base::trunc | std::ios_base::binary);
    }
    catch (const std::system_error &ex)
    {
        RaiseError("opening", ex);
    }

    // Preserve the open file stream to resume writing to when next chunk is available.
    m_ofs = std::move(ofs);
    // Preserve the relative path to the file to resume writing to when next chunk is available.
    m_name = name;
    return;
}

void SequentialFileWriter::RaiseError(const std::string action_attempted, const std::system_error &ex)
{
    const int ec = ex.code().value();
    SetNoSpace(ec);
    std::ostringstream sts;
    sts << "Error " << action_attempted << " the file " << m_name << ": ";
    raise_from_system_error_code(sts.str(), ec);
}

void SequentialFileWriter::SetNoSpace(const int ec)
{
    switch (ec)
    {
    case ENOSPC:
    case EFBIG:
        m_no_space = true;
        break;
    // TODO: Also handle permission errors.
    default:
        break;
    }
}