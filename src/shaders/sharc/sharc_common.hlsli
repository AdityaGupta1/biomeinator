// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#pragma once
#define SHARC_SEPARATE_EMISSIVE 1
#define SHARC_MATERIAL_DEMODULATION 1
#include "SharcCommon.h"
#include "../rendering/common/sharc_protocol.h"
#include "common/global_params.hlsli"

RWStructuredBuffer<uint64_t> sharcHashes : REGISTER_U(SHARC, HASHES);
RWStructuredBuffer<SharcAccumulationData> sharcAccumulation : REGISTER_U(SHARC, ACCUMULATION);
RWStructuredBuffer<SharcPackedData> sharcResolved : REGISTER_U(SHARC, RESOLVED);

SharcParameters makeSharcParameters()
{
    SharcParameters p;
    p.hashGridParameters.cameraPosition = sharcParams.cameraPosition;
    p.hashGridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
    p.hashGridParameters.sceneScale = sharcParams.sceneScale;
    p.hashGridParameters.levelBias = SHARC_GRID_LEVEL_BIAS;
    p.hashGridData.capacity = sharcParams.capacity;
    p.hashGridData.hashEntriesBuffer = sharcHashes;
    p.accumulationBuffer = sharcAccumulation;
    p.resolvedBuffer = sharcResolved;
    p.radianceScale = 1000.f;
    return p;
}
SharcHitData makeSharcHit(float3 position, float3 normal, float3 baseColor)
{
    SharcHitData hit;
    hit.positionWorld = position + float3(sharcParams.originDelta);
    hit.normalWorld = normal;
    hit.materialDemodulation = max(baseColor, 0.01f);
    hit.emissive = 0.f; // The renderer handles hit emission with its own MIS weight.
    return hit;
}
