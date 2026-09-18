// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once

#ifdef __cplusplus
#include <cstdint>
#endif

struct CloudSettings
{
    float coverage;
    float density;
    float baseHeight;
    float thickness;
    float cellSize;
    float period;
    float maxDistance;
    float marchDistance;
    float ambient;
    float phaseG;
    float windX;
    float windZ;
    float multiScatterStrength;
#ifdef __cplusplus
    uint32_t samples;
    uint32_t seed;
    uint32_t ser;
#else
    uint samples;
    uint seed;
    uint ser;
#endif
};
