#pragma once

#include <string>

// Get the basename of the given path
std::string extract_basename(const std::string& path);

// Raise a C++ system_error exception from the user-supplied error-code 'err', which
// should be a valid errno value
void raise_from_system_error_code [[noreturn]] (const std::string& user_message, int err);

// Raise a C++ system_error exception based on the current value of errno
void raise_from_errno [[noreturn]] (const std::string& user_message);
