#pragma once

#include <fstream>
#include <string>

#include "utils.h"

class SequentialFileWriter
{
public:
    SequentialFileWriter();
    SequentialFileWriter(SequentialFileWriter &&);
    SequentialFileWriter &operator=(SequentialFileWriter &&);

    // Write data from a string. On errors throws an exception drived from std::system_error
    // This method may take ownership of the string. Hence no assumption may be made about
    // the data it contains after it returns.
    /**
     * @brief Write the data represented as a string 'data' to the file at the relative path 'name'
     * @param name
     * @param data
     * @throws std::system_error fail bit | bad bit
     */
    void Write(const std::string &name, std::string &data);
    /**
     * @brief Indicates whether the writer has ran out of space for the file while writing. 
     * @return bool
     */
    bool NoSpace() const
    {
        return m_no_space;
    }

private:
    std::string m_name;
    std::ofstream m_ofs;
    bool m_no_space;

    /**
     * @brief Open the file at the relative path 'name', if it is not already open by this process.
     * @param name
     * @throws std::system_error fail bit | bad bit
     */
    void OpenIfNecessary(const std::string &name);
    /**
     * @brief Raises a given exception 'ex' caused by a given action 'action_attempted'.
     * @param action_attempted
     * @param ex
     */
    void RaiseError [[noreturn]] (const std::string action_attempted, const std::system_error &ex);
    /**
     * @brief Set the No Space flag according the error code 'ec'.
     * @param ec
     */
    void SetNoSpace (const int ec);
};
