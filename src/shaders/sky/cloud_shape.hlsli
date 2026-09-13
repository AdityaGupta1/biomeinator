// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once

static const uint cloudShapeSize = 1024;

float cloudShapeTexelSize()
{
    const float extent = 2.f * (renderParams.cloud.maxDistance + renderParams.cloud.marchDistance);
    return min(extent / float(cloudShapeSize), renderParams.cloud.period / 512.f);
}

float2 cloudShapeOrigin()
{
    const float2 camera = cameraParams.pos_WS.xz + float2(cameraParams.globalInstanceOffset.xz)
        - float2(renderParams.cloud.windX, renderParams.cloud.windZ) * renderParams.animTime;
    const float texel = cloudShapeTexelSize();
    return (floor(camera / texel) - float(cloudShapeSize / 2)) * texel;
}
