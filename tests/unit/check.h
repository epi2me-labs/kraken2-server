#pragma once
// A minimal test harness: CHECK macros that count failures, and finish() to
// report them. No framework, as the tests are small.
#include <iostream>
#include <sstream>

namespace testing_support {
inline int &failures() { static int n = 0; return n; }
}

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond \
                      << std::endl;                                              \
            testing_support::failures()++;                                       \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        auto va = (a);                                                           \
        auto vb = (b);                                                           \
        if (!(va == vb)) {                                                       \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: "    \
                      << #a << " == " << #b << " (" << va << " vs " << vb << ")" \
                      << std::endl;                                              \
            testing_support::failures()++;                                       \
        }                                                                        \
    } while (0)

// Returns the process exit status for main().
inline int finish(const char *name)
{
    if (testing_support::failures() == 0) {
        std::cout << name << ": passed" << std::endl;
        return 0;
    }
    std::cerr << name << ": " << testing_support::failures() << " check(s) FAILED" << std::endl;
    return 1;
}
