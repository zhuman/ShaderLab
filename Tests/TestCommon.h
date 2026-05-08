#pragma once

// Tiny shared header for ShaderLab test code split across multiple TUs.
// Owns the global pass / fail counters and the TEST() reporter, so
// Tests/TestRunner.cpp and Tests/Math/*.cpp can all log into the same
// summary line.

#include <cstdio>

namespace ShaderLab::Tests
{
    inline int g_passed = 0;
    inline int g_failed = 0;

    inline void TEST(const char* name, bool result)
    {
        if (result)
        {
            std::printf("  [PASS] %s\n", name);
            ++g_passed;
        }
        else
        {
            std::printf("  [FAIL] %s\n", name);
            ++g_failed;
        }
        std::fflush(stdout);
    }
}
