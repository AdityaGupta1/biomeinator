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
    float warpScale;
    float warpDetail;
    float warpRoughness;
    float warpStrength;
    float voronoiSmoothness;
    float voronoiRandomness;
    float fineScale;
    float fineDetail;
    float fineRoughness;
    float fineStrength;
    float heightRampEnd;
    float heightGain;
    float densityRampStart;
    float densityRampEnd;
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
    float warpTime;
    float warpSpeed;
    float voronoiTime;
    float voronoiSpeed;
    float fineTime;
    float fineSpeed;
    float padding0;
    float padding1;

};
