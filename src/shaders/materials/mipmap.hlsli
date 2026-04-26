// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#define TERRAIN_TILE_SIZE_TEXELS 16.f

struct RayCone
{
    float width;
    float angle;
};

float computeMipLevel(const float coneWidth)
{
    return log2(max(coneWidth, 1e-6f) * TERRAIN_TILE_SIZE_TEXELS) + renderParams.mipBias;
}

float getRayConePixelAngle()
{
    return 2.f * atan(cameraParams.tanHalfFovY) / float(renderParams.renderSize.y);
}

float getRayConeWidthAtDistance(const RayCone rayCone, const float distanceTraveled)
{
    return rayCone.width + rayCone.angle * distanceTraveled;
}
