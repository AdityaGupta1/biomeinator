// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#if ENABLE_ASSERTS
#include "logger.h"
#include <cassert>

#define ASSERT_IMPL(cond, msg)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            Logger::logError("Assertion failed: %s", #cond);                                                           \
            if constexpr (sizeof(msg) > 1)                                                                             \
            {                                                                                                          \
                Logger::logError("Message: %s", msg);                                                                  \
            }                                                                                                          \
            Logger::logError("File: %s, line: %d", __FILE__, __LINE__);                                                \
            __debugbreak();                                                                                            \
        }                                                                                                              \
    } while (0)

#define ASSERT(cond, ...) ASSERT_IMPL(cond, "" __VA_ARGS__)
#else
#define ASSERT(cond, ...) ((void)0)
#endif
