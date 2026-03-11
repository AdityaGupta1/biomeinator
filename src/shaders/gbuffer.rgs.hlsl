/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

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

#include "../rendering/common/common_hitgroups.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_structs.h"

#define HITGROUP_LIGHTS GBUFFER_HITGROUP_LIGHTS

#include "global_params.hlsli"
#include "materials.hlsli"
#include "path_tracing_common.hlsli"
#include "payload.hlsli"
#include "ris.hlsli"
#include "util/color.hlsli"
#include "util/rng.hlsli"

RWStructuredBuffer<GbufferData> gbufferOut : REGISTER_U(GBUFFER, GBUFFER_OUT);

// motion is in uv space, not pixel space
float2 calculateMotionFromPos(const float3 pos_WS)
{
    float4 currNdc = mul(cameraParams.worldToClipMat, float4(pos_WS, 1));
    currNdc /= currNdc.w;
    float4 prevNdc = mul(cameraParams.worldToPrevClipMat, float4(pos_WS, 1)); // worldToPrevClipMat accounts for changed globalInstanceOffset
    prevNdc /= prevNdc.w;

    float2 motion = (prevNdc.xy - currNdc.xy) / 2.f;
    motion.y = -motion.y;
    return motion;
}

void outputGuideBuffers(const Payload payload, const RayDesc ray)
{
    const uint2 pixelIdx = DispatchRaysIndex().xy;

    float linearDepth = cameraParams.farPlane;
    float3 motionHitPos_WS;
    float3 hitNor_WS = 0.f;
    float roughness = 0.f;
    float3 specularAlbedo = 0.f;

    // diffuse albedo is written in path_tracing.rgs.hlsl so it can be modulated by specular bounces

    if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
    {
        linearDepth = distance(ray.Origin, payload.hitInfo.hitPos_WS);

        motionHitPos_WS = payload.hitInfo.hitPos_WS;
        hitNor_WS = payload.hitInfo.hitNor_WS;

        // TODO: eventually set roughness

        if (payload.materialIdx != MATERIAL_IDX_INVALID)
        {
            const Material surfMaterial = getMaterialFromPayload(payload);

            if (surfMaterial.hasGlossyReflection())
            {
                const float alpha = roughness * roughness;
                const float nDotV = dot(hitNor_WS, -ray.Direction);
                specularAlbedo = calculateDlssSpecularAlbedo(surfMaterial.glossyReflectionTint, alpha, nDotV);
            }
            else
            {
                roughness = 1.f; // for now, roughness is 0.f for all materials that have glossy reflection and 1.f otherwise
            }
        }
    }
    else
    {
        motionHitPos_WS = evalRayPos(ray, cameraParams.farPlane);
        hitNor_WS = normalize(-ray.Direction);
    }

    RWTexture2D<float> linearDepthTarget = ResourceDescriptorHeap[heapIndices.uav.linearDepthTargetIdx];
    linearDepthTarget[pixelIdx] = linearDepth;

    RWTexture2D<float2> motionTarget = ResourceDescriptorHeap[heapIndices.uav.motionTargetIdx];
    motionTarget[pixelIdx] = calculateMotionFromPos(motionHitPos_WS);

    RWTexture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.uav.normalsAndRoughnessTargetIdx];
    normalsAndRoughnessTarget[pixelIdx].xyzw = float4(hitNor_WS, roughness);

    RWTexture2D<float4> specularAlbedoTarget = ResourceDescriptorHeap[heapIndices.uav.specularAlbedoTargetIdx];
    specularAlbedoTarget[pixelIdx] = float4(specularAlbedo, 1);

    RWTexture2D<float> specularHitDistanceTarget = ResourceDescriptorHeap[heapIndices.uav.specularHitDistanceTargetIdx];
    specularHitDistanceTarget[pixelIdx] = 0; // will be overwritten in path tracing pass
}

[shader("raygeneration")]
void RayGeneration()
{
    const uint2 pixelIdx = DispatchRaysIndex().xy;
    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;

    RayDesc ray;
    ray.Origin = cameraParams.pos_WS;
    ray.Direction = getPrimaryRayDirection(pixelIdx);
    ray.TMin = 0.001f;
    ray.TMax = RAY_DEFAULT_TMAX;

    Payload payload;
    payload.materialIdx = MATERIAL_IDX_INVALID;
    payload.flags = (sceneParams.cameraUnderwater ? PAYLOAD_FLAG_UNDERWATER : 0) | PAYLOAD_FLAG_IS_GBUFFER;
    payload.rng = initRng(constantParams.rngSeed, 123909203, linearPixelIdx, renderParams.frameNumber);
    payload.waterEntryT = RAY_DEFAULT_TMAX;
    payload.waterExitT = RAY_DEFAULT_TMAX;
    payload.coneWidth = 0.f;
    payload.coneSurfaceSpreadAngle = 0.f;

    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, GBUFFER_HITGROUP_PRIMARY, 0, 0, ray, payload);

    outputGuideBuffers(payload, ray);

    GbufferData outGbufferData;
    outGbufferData.hitInfo = payload.hitInfo;
    outGbufferData.materialIdx = payload.materialIdx;
    outGbufferData.payloadFlags = payload.flags;
    outGbufferData.pad0 = outGbufferData.pad1 = 0;
    gbufferOut[linearPixelIdx] = outGbufferData;
}
