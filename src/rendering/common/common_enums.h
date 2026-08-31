// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#ifdef __cplusplus
#include <stdint.h>

#define uint uint32_t
#endif

enum class Tonemapping : uint
{
    NONE,
    STANDARD,
    AGX,
    KHRONOS_PBR_NEUTRAL,

    COUNT
};

enum class AntialiasingMode : uint
{
    NONE,
    ACCUMULATE,
    DLSS,

    COUNT
};

enum class SamplingMode : uint
{
    NAIVE,
    MIS,
    RTSL,

    COUNT
};

#ifdef __cplusplus
#undef uint
#endif
