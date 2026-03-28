/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2026 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_structs.h"

#include "global_params.hlsli"
#include "radiance_cache.hlsli"
#include "util/color.hlsli"
#include "util/ray.hlsli"

SamplerState texSampler : REGISTER_S(POSTPROCESS, TEX_SAMPLER);

ByteAddressBuffer rcHashEntries : REGISTER_T(RC, HASH_ENTRIES);
StructuredBuffer<float4> rcResolved : REGISTER_T(RC, RESOLVED);

struct PsIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 reconstructWorldPos(float2 uv)
{
    Texture2D<float> linearDepthTex = ResourceDescriptorHeap[heapIndices.srv.linearDepthTargetIdx];
    const float linearDepth = linearDepthTex.SampleLevel(texSampler, uv, 0);

    const uint2 pixelIdx = uint2(uv * float2(renderParams.renderSize));
    const float3 rayDir = getPrimaryRayDirection(pixelIdx);

    return evalRayPos(cameraParams.pos_WS, rayDir, linearDepth);
}

float4 getRcDebugColor(float2 uv)
{
    if (!rcParams.rcEnabled)
    {
        return float4(0, 0, 0, 1);
    }

    const float3 worldPos = reconstructWorldPos(uv);

    if (debugParams.rcDebugView == 1)
    {
        const int level = rcGetLevel(worldPos);
        const int3 gridPos = rcWorldToGrid(worldPos, level);
        const uint rcHash = rcSpatialHash(gridPos, level);
        RandomNumberGenerator rng = initRng(rcHash, 105691202);
        return float4(0.2f + 0.8f * rng.nextFloat3(), 1.f);
    }
    else // rcDebugView == 2
    {
        const uint2 pixelIdx = uint2(uv * float2(renderParams.renderSize));
        RandomNumberGenerator rng = initRng(pixelIdx.x, pixelIdx.y, renderParams.frameNumber);
        const int level = rcGetLevel(worldPos);
        const int3 gridPos = rcWorldToGrid(rcJitterPos(worldPos, level, rng), level);
        const uint slot = rcLookup(gridPos, level, rcHashEntries);
        if (slot == RC_INVALID_SLOT)
        {
            return float4(0, 0, 0, 1);
        }

        const float4 resolved = rcResolved[slot];
        if (resolved.w < (float)rcParams.rcMinSamplesForQuery)
        {
            return float4(0.1, 0, 0.1, 1); // dim magenta = populated but under-sampled
        }

        return float4(resolved.rgb, 1);
    }
}

float4 getDebugColor(float2 uv)
{
    float4 debugColor = 0;

    switch (debugParams.debugOutputNumChannels)
    {
        case 4:
        {
            Texture2D<float4> debugTexture = ResourceDescriptorHeap[debugParams.debugOutputSrvIdx];
            debugColor = debugTexture.Sample(texSampler, uv).rgba;
            break;
        }
        case 3:
        {
            Texture2D<float4> debugTexture = ResourceDescriptorHeap[debugParams.debugOutputSrvIdx];
            debugColor = float4(debugTexture.Sample(texSampler, uv).rgb, 1);
            break;
        }
        case 2:
        {
            Texture2D<float2> debugTexture = ResourceDescriptorHeap[debugParams.debugOutputSrvIdx];
            debugColor = float4(debugTexture.Sample(texSampler, uv).rg, 0, 1);
            break;
        }
        case 1:
        {
            Texture2D<float> debugTexture = ResourceDescriptorHeap[debugParams.debugOutputSrvIdx];
            debugColor = float4(debugTexture.Sample(texSampler, uv).rrr, 1);
            break;
        }
    }

    return debugColor;
}

float4 psMain(PsIn psIn) : SV_Target
{
    float4 debugColor;

    if (debugParams.rcDebugView != 0)
    {
        debugColor = getRcDebugColor(psIn.uv);
    }
    else
    {
        debugColor = getDebugColor(psIn.uv);
    }

    if (debugParams.debugViewApplyTonemap)
    {
        debugColor.rgb = applyTonemapping(debugColor.rgb);
    }

    debugColor.rgb *= debugParams.debugOutputScale;

    return debugColor;
}
