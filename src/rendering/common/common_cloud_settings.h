// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once

struct CloudSettings
{
    float coverage;
    float density;
    float baseHeight;
    float thickness;
    float period;
    float maxDistance;
    float marchDistance;
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
    float bottomWidth;
    float bottomGain;
    float densityRampStart;
    float densityRampEnd;
    float ambient;
    float phaseG;
    float windX;
    float windZ;
    float warpTime;
    float warpSpeed;
    float voronoiTime;
    float voronoiSpeed;
    float fineTime;
    float fineSpeed;
    float stepSize;
    float secondaryStepSize;
    float lightStepSize;
    float multiScatter;
    float padding0;
    float padding1;
    float padding2;
};
