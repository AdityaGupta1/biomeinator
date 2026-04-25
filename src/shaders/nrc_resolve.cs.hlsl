// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"

#define NRC_RW_STRUCTURED_BUFFER(T) RWStructuredBuffer<T>

#include "NrcHelpers.hlsli"

cbuffer NrcConstantBuffer : REGISTER_B(NRC, NRC_CONSTANTS)
{
    NrcConstants nrcConstants;
};

RWStructuredBuffer<NrcPackedQueryPathInfo> nrcQueryPathInfo : REGISTER_U(NRC, QUERY_PATH_INFO);
RWStructuredBuffer<float3> nrcQueryRadiance : REGISTER_U(NRC, QUERY_RADIANCE);
RWStructuredBuffer<float4> pathTracingRawBufferOut : REGISTER_U(PT, PATH_TRACING_RAW_BUFFER_OUT);

[shader("compute")]
[numthreads(NRC_RESOLVE_WORKGROUP_SIZE_X, NRC_RESOLVE_WORKGROUP_SIZE_Y, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixelIdx = dispatchThreadId.xy;
    if (pixelIdx.x >= nrcConstants.frameDimensions.x || pixelIdx.y >= nrcConstants.frameDimensions.y)
    {
        return;
    }

    const uint pathInfoIndex = NrcCalculateQueryPathIndex(nrcConstants.frameDimensions, pixelIdx, 0, nrcConstants.samplesPerPixel);
    const NrcQueryPathInfo pathInfo = NrcUnpackQueryPathInfo(nrcQueryPathInfo[pathInfoIndex]);
    if (pathInfo.queryBufferIndex == 0xFFFFFFFF)
    {
        return;
    }

    const float3 queryRadiance = NrcUnpackQueryRadiance(nrcConstants, nrcQueryRadiance[pathInfo.queryBufferIndex]);
    const float3 resolvedRadiance = pathInfo.prefixThroughput * queryRadiance;
    const uint bufferIdx = pixelIdx.y * nrcConstants.frameDimensions.x + pixelIdx.x;

    float4 pathTracingRaw = pathTracingRawBufferOut[bufferIdx];
    pathTracingRaw.rgb += resolvedRadiance;
    pathTracingRawBufferOut[bufferIdx] = pathTracingRaw;
}
