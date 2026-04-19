// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "debug.h"

namespace MathUtil
{

inline int floorDiv(int a, int d)
{
    ASSERT(d > 0);
    return (a / d) - ((a ^ d) < 0 && a % d != 0);
}

inline constexpr bool isPowerOfTwo(size_t x)
{
    return x > 0 && (x & (x - 1)) == 0;
}

inline constexpr size_t roundUp(size_t size, size_t multiple)
{
    ASSERT(isPowerOfTwo(multiple));
    return (size + multiple - 1) & ~(multiple - 1);
}

}
