// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "util/math.hlsli"

// Translation that brings a position stored in the previous frame's render space into this frame's,
// nonzero only when the voxel floating origin moved between the frames
float3 prevFrameToCurrentOffset()
{
    return float3(cameraParams.prevGlobalInstanceOffset - cameraParams.globalInstanceOffset);
}

// Finds the previous frame's pixel that saw this frame's primary hit and checks that it saw the same
// surface: the previous hit must exist, lie within a few pixel footprints of the current one and face
// the same way. A pixel is identified by its area, ignoring jitter: with a still camera every pixel
// reprojects onto itself, otherwise sub-pixel jitter would pick a neighbor whenever it shrank and the
// reused history would visibly drift.
bool reprojectToPrevPixel(const HitInfo hitInfo, StructuredBuffer<GbufferData> gbufferPrevIn, out uint2 prevPixelIdx)
{
    prevPixelIdx = 0;

    const float4 prevClip = mul(cameraParams.worldToPrevClipMat, float4(hitInfo.hitPos_WS, 1.f));
    if (prevClip.w <= 0.f)
    {
        return false;
    }
    const float2 prevNdc = prevClip.xy / prevClip.w;
    const float2 prevUv = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
    const float2 prevPixel = prevUv * float2(renderParams.renderSize);
    if (any(prevPixel < 0.f) || any(prevPixel >= float2(renderParams.renderSize)))
    {
        return false;
    }
    prevPixelIdx = uint2(prevPixel);

    const GbufferData prevGbuffer = gbufferPrevIn[prevPixelIdx.y * renderParams.renderSize.x + prevPixelIdx.x];
    if (!bool(prevGbuffer.payloadFlags & PAYLOAD_FLAG_DID_HIT) || prevGbuffer.materialIdx == MATERIAL_IDX_INVALID)
    {
        return false;
    }

    const float distToCamera = distance(cameraParams.pos_WS, hitInfo.hitPos_WS);
    const float pixelFootprint = distToCamera * 2.f * cameraParams.tanHalfFovY / float(renderParams.renderSize.y);
    const float3 prevHitPos_WS = prevGbuffer.hitInfo.hitPos_WS + prevFrameToCurrentOffset();
    if (distance(prevHitPos_WS, hitInfo.hitPos_WS) > 4.f * pixelFootprint)
    {
        return false;
    }
    return dot(prevGbuffer.hitInfo.hitNor_WS, hitInfo.hitNor_WS) > 0.9f;
}
