// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_hitgroups.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "common/path_tracing_common.hlsli"
#include "common/payload.hlsli"
#include "materials/materials.hlsli"
#include "util/color.hlsli"
#include "util/rng.hlsli"

RWStructuredBuffer<GbufferData> gbufferOut : REGISTER_U(GBUFFER, GBUFFER_OUT);

float3 calculateNdc(const float4x4 worldToClipMat, const float3 pos_WS)
{
    const float4 clip = mul(worldToClipMat, float4(pos_WS, 1));
    return clip.xyz / clip.w;
}

// motion is in uv space, not pixel space
float2 calculateMotionFromNdc(const float3 currNdc, const float3 prevPos_WS)
{
    const float3 prevNdc = calculateNdc(cameraParams.worldToPrevClipMat, prevPos_WS); // worldToPrevClipMat accounts for changed globalInstanceOffset

    float2 motion = (prevNdc.xy - currNdc.xy) / 2.f;
    motion.y = -motion.y;
    return motion;
}

void outputGuideBuffers(const Payload payload, const RayDesc ray)
{
    const uint2 pixelIdx = DispatchRaysIndex().xy;

    float3 motionHitPos_WS;
    float3 prevMotionHitPos_WS;
    float3 hitShadingNor_WS = 0.f;
    float roughness = 0.f;
    float3 specularAlbedo = 0.f;

    // diffuse albedo is written in path_tracing.rgs.hlsl so it can be modulated by specular bounces, which
    // also overwrites the specular albedo below for a rough glossy first hit (see computeFirstBounceAlbedos)

    if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
    {
        motionHitPos_WS = payload.hitInfo.hitPos_WS;
        prevMotionHitPos_WS = motionHitPos_WS;
        hitShadingNor_WS = payload.hitInfo.hitShadingNor_WS;

        // water displacement is vertical at fixed XZ, so the previous position of a water
        // surface point is the same column's wave height at the previous frame's time
        const InstanceData instanceData = instanceDatas[payload.hitInfo.instanceId];
        const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + payload.hitInfo.triangleIdx];
        if (bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER_TOP))
        {
            const float2 posXZ_WS = motionHitPos_WS.xz + float2(cameraParams.globalInstanceOffset.xz);
            prevMotionHitPos_WS.y += waveHeight(posXZ_WS, renderParams.prevAnimTime) - waveHeight(posXZ_WS, renderParams.animTime);
        }

        if (payload.materialIdx != MATERIAL_IDX_INVALID)
        {
            const float coneWidth = getRayConeWidthAtDistance(payload.rayCone, distance(ray.Origin, payload.hitInfo.hitPos_WS));
            Material surfMaterial = getHitMaterial(payload, coneWidth);

            if (surfMaterial.hasGlossyReflection())
            {
                roughness = surfMaterial.roughness;
                const float alpha = roughness * roughness;
                const float nDotV = dot(hitShadingNor_WS, -ray.Direction);
                specularAlbedo = calculateDlssSpecularAlbedo(surfMaterial.glossyReflectionTint, alpha, nDotV);
            }
            else
            {
                roughness = 1.f;
            }
        }
    }
    else
    {
        // Put the sky on the far plane itself, not on a sphere of radius farPlane, so its depth is 1
        // everywhere rather than falling off towards the screen edges
        const float distToFarPlane = cameraParams.farPlane / dot(ray.Direction, cameraParams.forward_WS);
        motionHitPos_WS = evalRayPos(ray, distToFarPlane);
        prevMotionHitPos_WS = motionHitPos_WS;
        hitShadingNor_WS = normalize(-ray.Direction);
    }

    const float3 currNdc = calculateNdc(cameraParams.worldToClipMat, motionHitPos_WS);

    RWTexture2D<float> depthTarget = ResourceDescriptorHeap[heapIndices.uav.depthTargetIdx];
    depthTarget[pixelIdx] = currNdc.z;

    RWTexture2D<float2> motionTarget = ResourceDescriptorHeap[heapIndices.uav.motionTargetIdx];
    motionTarget[pixelIdx] = calculateMotionFromNdc(currNdc, prevMotionHitPos_WS);

    RWTexture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.uav.normalsAndRoughnessTargetIdx];
    normalsAndRoughnessTarget[pixelIdx].xyzw = float4(hitShadingNor_WS, roughness);

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
    payload.rayCone.width = 0.f;
    payload.rayCone.angle = getRayConePixelAngle();

    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, HITGROUP_PRIMARY, 0, 0, ray, payload);

    outputGuideBuffers(payload, ray);

    GbufferData outGbufferData;
    outGbufferData.hitInfo = payload.hitInfo;
    outGbufferData.materialIdx = payload.materialIdx;
    outGbufferData.payloadFlags = payload.flags;
    outGbufferData.pad0 = outGbufferData.pad1 = 0; // necessary since we're writing to a UAV
    gbufferOut[linearPixelIdx] = outGbufferData;
}
