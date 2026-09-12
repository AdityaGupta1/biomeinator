// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once

// Float-only layout is identical in root constants and the global parameter block.
struct CloudSettings
{
    float baseHeight;
    float thickness;
    float period;
    float maxDistance;
    float weatherScale;
    float billowScale;
    float perlinWeight;
    float shapeThreshold;
    float shapeGain;
    float weatherSoftness;
    float bottomFade;
    float topStart;
    float topVariance;
    float mediumRepeats;
    float fineRepeats;
    float mediumErosion;
    float fineErosion;
    float warpStrength;
    float ambient;
    float phaseG;
    float multiScatter;
    float powder;
    float aerial;
    float windX;
    float windZ;
    float lightSteps;
    float secondarySteps;
    float viewDownscale;
};
