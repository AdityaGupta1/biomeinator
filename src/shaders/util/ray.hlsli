// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "global_params.hlsli"
#include "util/math.hlsli"

#define RAY_DEFAULT_TMAX 10000.f
#define RAY_ORIGIN_OFFSET_EPSILON 0.0001f

float3 getPrimaryRayDirection(const uint2 pixelIdx)
{
    const float2 size = float2(renderParams.renderSize);

    const float2 uv = (pixelIdx + cameraParams.jitter) / size;
    const float2 ndc = float2(uv.x * 2.f - 1.f, 1.f - uv.y * 2.f);

    const float aspect = size.x / size.y;
    const float yScale = cameraParams.tanHalfFovY;
    const float xScale = yScale * aspect;

    const float3 targetPos_WS = cameraParams.pos_WS
        + cameraParams.right_WS * ndc.x * xScale
        + cameraParams.up_WS * ndc.y * yScale
        + cameraParams.forward_WS;
    return normalize(targetPos_WS - cameraParams.pos_WS);
}

float3 getPrevPrimaryRayDirection(const uint2 pixelIdx)
{
    const float2 size = float2(renderParams.renderSize);

    const float2 uv = (pixelIdx + cameraParams.prevJitter) / size;
    const float2 ndc = float2(uv.x * 2.f - 1.f, 1.f - uv.y * 2.f);

    const float aspect = size.x / size.y;
    const float yScale = cameraParams.prevTanHalfFovY;
    const float xScale = yScale * aspect;

    const float3 targetPos_WS = cameraParams.prevPos_WS
        + cameraParams.prevRight_WS * ndc.x * xScale
        + cameraParams.prevUp_WS * ndc.y * yScale
        + cameraParams.prevForward_WS;
    return normalize(targetPos_WS - cameraParams.prevPos_WS);
}

void setRayOriginAndDirection(inout RayDesc ray, const float3 origin_WS, float3 normal_WS, const float3 wi_WS, bool faceforwardNormal)
{
    if (faceforwardNormal)
    {
        normal_WS = faceforward(normal_WS, wi_WS);
    }

    ray.Origin = mad(normal_WS, RAY_ORIGIN_OFFSET_EPSILON, origin_WS);
    ray.Direction = wi_WS;
}

float3 evalRayPos(const float3 origin, const float3 direction, const float t)
{
    return mad(direction, t, origin);
}

float3 evalRayPos(const RayDesc ray, const float t)
{
    return evalRayPos(ray.Origin, ray.Direction, t);
}
