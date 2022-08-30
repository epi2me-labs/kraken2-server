#include <sstream>
#include <stdexcept>
#include <cassert>

// Select GNU basename() on Linux
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <limits.h>
#include <libgen.h>
#include <string.h>
#include <stdlib.h>

#include "utils.h"

std::string extract_basename(const std::string& path)
{
    char* const temp_s = strdup(path.c_str());
    std::string result(basename(temp_s));
    free(temp_s);
    return result;
}

void raise_from_system_error_code(const std::string& user_message, int err)
{
    std::ostringstream sts;
    if (user_message.size() > 0) {
        sts << user_message << ' ';
    }

    assert(0 != err);
    throw std::system_error(std::error_code(err, std::system_category()), sts.str().c_str());
}

void raise_from_errno(const std::string& user_message)
{
    raise_from_system_error_code(user_message, errno);
}
