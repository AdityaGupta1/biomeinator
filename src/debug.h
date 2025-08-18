#pragma once

#ifdef _DEBUG
#include <iostream>

#define ASSERT_IMPL(cond, msg)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::cerr << "Assertion failed: (" << #cond << ")\n";                                                      \
            if constexpr (sizeof(msg) > 1)                                                                             \
            {                                                                                                          \
                std::cerr << "Message: " << msg << "\n";                                                               \
            }                                                                                                          \
            std::cerr << "File: " << __FILE__ << ", line: " << __LINE__ << std::endl;                                  \
            __debugbreak();                                                                                            \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT(cond, ...) ASSERT_IMPL(cond, "" __VA_ARGS__)
#else
#define ASSERT(cond, ...) ((void)0)
#endif