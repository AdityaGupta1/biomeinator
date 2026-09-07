// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_enums.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "util/packing.hlsli"
#include "util/rng.hlsli"

StructuredBuffer<float4> pathTracingRawBufferIn : REGISTER_T(COLLECT, PATH_TRACING_RAW_BUFFER_IN);
StructuredBuffer<float4> ptDiffuseAlbedoRawBufferIn : REGISTER_T(COLLECT, PT_DIFFUSE_ALBEDO_RAW_BUFFER_IN);

[shader("compute")]
[numthreads(COLLECT_WORKGROUP_SIZE_X, COLLECT_WORKGROUP_SIZE_Y, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixelIdx = dispatchThreadId.xy;

    if (pixelIdx.x >= renderParams.renderSize.x || pixelIdx.y >= renderParams.renderSize.y)
    {
        return;
    }

    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;

    float3 color, diffuseAlbedo;
    if (bool(renderParams.doPathSplitting))
    {
        const uint bufferIdx0 = linearPixelIdx * 2;
        const uint bufferIdx1 = bufferIdx0 + 1;

        color = pathTracingRawBufferIn[bufferIdx0].rgb + pathTracingRawBufferIn[bufferIdx1].rgb;
        diffuseAlbedo = ptDiffuseAlbedoRawBufferIn[bufferIdx0].rgb + ptDiffuseAlbedoRawBufferIn[bufferIdx1].rgb;
    }
    else
    {
        color = pathTracingRawBufferIn[linearPixelIdx].rgb;
        diffuseAlbedo = ptDiffuseAlbedoRawBufferIn[linearPixelIdx].rgb;
    }

    if ((AntialiasingMode) renderParams.antialiasingMode == AntialiasingMode::ACCUMULATE)
    {
        color /= (renderParams.accumulatedFrameNumber + 1.f);
    }

    // ReSTIR's temporally filtered output starves the denoiser of the per-frame noise it is trained
    // on; a little zero-mean noise relative to the pixel's own value reads as path tracing noise to it
    if ((SamplingMode)renderParams.samplingMode == SamplingMode::RESTIR_PT && restirParams.whiteNoise > 0.f)
    {
        RandomNumberGenerator rng = initRng(constantParams.rngSeed, 31415927, linearPixelIdx, renderParams.frameNumber);
        color *= max(1.f + restirParams.whiteNoise * (2.f * rng.nextFloat() - 1.f), 0.f);
    }

    RWTexture2D<float4> pathTracingTarget = ResourceDescriptorHeap[heapIndices.uav.pathTracingTargetIdx];
    pathTracingTarget[pixelIdx] = float4(color, 1.f);

    RWTexture2D<float4> diffuseAlbedoTarget = ResourceDescriptorHeap[heapIndices.uav.diffuseAlbedoTargetIdx];
    diffuseAlbedoTarget[pixelIdx] = float4(diffuseAlbedo, 1.f);
}
