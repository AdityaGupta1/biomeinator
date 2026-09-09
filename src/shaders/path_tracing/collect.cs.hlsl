// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_enums.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "util/blue_noise.hlsli"
#include "util/packing.hlsli"

StructuredBuffer<float4> pathTracingRawBufferIn : REGISTER_T(COLLECT, PATH_TRACING_RAW_BUFFER_IN);
StructuredBuffer<float4> ptDiffuseAlbedoRawBufferIn : REGISTER_T(COLLECT, PT_DIFFUSE_ALBEDO_RAW_BUFFER_IN);

// A pixel's raw path traced color, both split slots summed
float3 rawColor(const uint2 pixelIdx)
{
    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;
    if (bool(renderParams.doPathSplitting))
    {
        return pathTracingRawBufferIn[linearPixelIdx * 2].rgb + pathTracingRawBufferIn[linearPixelIdx * 2 + 1].rgb;
    }
    return pathTracingRawBufferIn[linearPixelIdx].rgb;
}

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

    float3 color = rawColor(pixelIdx);
    float3 diffuseAlbedo;
    if (bool(renderParams.doPathSplitting))
    {
        diffuseAlbedo = ptDiffuseAlbedoRawBufferIn[linearPixelIdx * 2].rgb + ptDiffuseAlbedoRawBufferIn[linearPixelIdx * 2 + 1].rgb;
    }
    else
    {
        diffuseAlbedo = ptDiffuseAlbedoRawBufferIn[linearPixelIdx].rgb;
    }

    if ((AntialiasingMode) renderParams.antialiasingMode == AntialiasingMode::ACCUMULATE)
    {
        color /= (renderParams.accumulatedFrameNumber + 1.f);
    }

    // Dropout (DLSS-RR integration guide, Section 3.5): zero a blue-noise-chosen fraction of the
    // pixels and rescale the survivors, which keeps the mean while breaking the correlation that
    // ReSTIR's reuse leaves between neighbors and frames, which the denoiser otherwise reads as
    // detail
    if ((SamplingMode)renderParams.samplingMode == SamplingMode::RESTIR_PT && restirParams.dropoutProbability > 0.f)
    {
        const bool dropped = blueNoise(pixelIdx, renderParams.frameNumber) < restirParams.dropoutProbability;
        color = dropped ? 0.f : color / (1.f - restirParams.dropoutProbability);
    }

    RWTexture2D<float4> pathTracingTarget = ResourceDescriptorHeap[heapIndices.uav.pathTracingTargetIdx];
    pathTracingTarget[pixelIdx] = float4(color, 1.f);

    RWTexture2D<float4> diffuseAlbedoTarget = ResourceDescriptorHeap[heapIndices.uav.diffuseAlbedoTargetIdx];
    diffuseAlbedoTarget[pixelIdx] = float4(diffuseAlbedo, 1.f);
}
