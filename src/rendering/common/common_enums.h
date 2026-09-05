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
    RESTIR_PT, // RTSL NEE plus path resampling

    COUNT
};

// Which raygen the path tracing pipeline runs (PassConstants in path_tracing.rgs.hlsl)
enum class PtPass : uint
{
    INITIAL_SAMPLING,
    SPATIAL_SHIFT,

    COUNT
};

enum class RestirDebugMode : uint
{
    OFF,
    SELF_REPLAY,       // shade with the selected path re-traced from its own reservoir instead of the stored F
    SELF_REPLAY_ERROR, // relative error of the re-traced F against the stored F, scaled by 100
    SPATIAL_SELF,      // spatial reuse with every pixel paired to itself; must reproduce no reuse exactly

    COUNT
};

#ifdef __cplusplus
#undef uint
#endif
