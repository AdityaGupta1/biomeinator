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

#define WATER_SIGMA_A (float3(0.35f, 0.06f, 0.02f) * 0.4f)

// Media that fill voxel cells, one row each: name, IOR, and absorption coefficient per block. The
// Medium enum, the shaders' per-medium tables and the block JSON names are all generated from this list,
// so a medium is added in one place. Only the shaders expand the absorption, so it can use HLSL types.
#define MEDIA_TABLE(X) \
    X(AIR, 1.f, float3(0.f, 0.f, 0.f)) \
    X(WATER, 1.33f, WATER_SIGMA_A) \
    X(ICE, 1.31f, WATER_SIGMA_A) /* absorbs about as much as water */ \
    X(GLASS, 1.55f, float3(0.f, 0.f, 0.f)) /* quartz-ish */

#define MEDIUM_ENUM_ENTRY(name, ior, sigmaA) name,
// Each terrain face records the medium in front of it (the side its normal points to) and behind it;
// equal media mean the face is not a medium boundary.
enum class Medium : uint
{
    MEDIA_TABLE(MEDIUM_ENUM_ENTRY)

    COUNT
};
#undef MEDIUM_ENUM_ENTRY

#ifdef __cplusplus
#undef uint
#endif
