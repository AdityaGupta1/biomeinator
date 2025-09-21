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

#include "global_params.hlsli"
#include "materials.hlsli"
#include "path_tracing_common.hlsli"
#include "payload.hlsli"

RWStructuredBuffer<GbufferData> gbuffer : REGISTER_U(GBUFFER_REGISTER_GBUFFER, GBUFFER_REGISTER_SPACE);

float2 calculateMotionFromPos(const float3 pos_WS)
{
    float4 currNdc = mul(cameraParams.worldToClipMat, float4(pos_WS, 1));
    currNdc /= currNdc.w;
    float4 prevNdc = mul(cameraParams.worldToPrevClipMat, float4(pos_WS, 1));
    prevNdc /= prevNdc.w;

    float2 motion = (prevNdc.xy - currNdc.xy) / 2;
    motion.y = -motion.y;
    //motion *= DispatchRaysDimensions().xy;
    return motion;
}

void outputGuideBuffers(const Payload payload, const RayDesc ray)
{
    const uint2 pixelIdx = DispatchRaysIndex().xy;

    float3 diffuseAlbedo = 0.f;
    float linearDepth = cameraParams.farPlane;
    float3 motionHitPos_WS;
    float3 hitNor_WS = 0.f;
    float roughness = 0.f;
    float3 specularAlbedo = 0.f;

    if (bool(payload.flags & PAYLOAD_FLAG_DID_HIT))
    {
        const Material surfMaterial = materials[payload.materialIdx];
        diffuseAlbedo = getMaterialDiffuseAlbedo(surfMaterial, payload.hitInfo.uv);

        linearDepth = distance(ray.Origin, payload.hitInfo.hitPos_WS);

        motionHitPos_WS = payload.hitInfo.hitPos_WS;
        hitNor_WS = payload.hitInfo.hitNor_WS;

        // TODO: eventually set roughness

        if (surfMaterial.hasSpecularReflection())
        {
            const float alpha = roughness * roughness;
            const float nDotV = dot(hitNor_WS, -ray.Direction);
            specularAlbedo = calculateDlssSpecularAlbedo(surfMaterial.specularColor, alpha, nDotV);
        }
    }
    else
    {
        motionHitPos_WS = evalRayPos(ray, cameraParams.farPlane);
        hitNor_WS = normalize(-ray.Direction);

        specularAlbedo = 0.5f; // this was suggested somewhere for miss specular albedo (I forgot where though)
    }

    RWTexture2D<float4> diffuseAlbedoTarget = ResourceDescriptorHeap[heapIndices.uav.diffuseAlbedoTargetIdx];
    diffuseAlbedoTarget[pixelIdx] = float4(diffuseAlbedo, 1);

    RWTexture2D<float> linearDepthTarget = ResourceDescriptorHeap[heapIndices.uav.linearDepthTargetIdx];
    linearDepthTarget[pixelIdx] = linearDepth;

    RWTexture2D<float2> motionTarget = ResourceDescriptorHeap[heapIndices.uav.motionTargetIdx];
    motionTarget[pixelIdx] = calculateMotionFromPos(motionHitPos_WS);

    RWTexture2D<float4> normalsAndRoughnessTarget = ResourceDescriptorHeap[heapIndices.uav.normalsAndRoughnessTargetIdx];
    normalsAndRoughnessTarget[pixelIdx].xyzw = float4(hitNor_WS, roughness);

    RWTexture2D<float4> specularAlbedoTarget = ResourceDescriptorHeap[heapIndices.uav.specularAlbedoTargetIdx];
    specularAlbedoTarget[pixelIdx] = float4(specularAlbedo, 1);
}

[shader("raygeneration")]
void RayGeneration()
{
    const uint2 pixelIdx = DispatchRaysIndex().xy;
    const uint linearPixelIdx = pixelIdx.y * DispatchRaysDimensions().x + pixelIdx.x;

    RayDesc ray;
    ray.Origin = cameraParams.pos_WS;
    ray.Direction = getPrimaryRayDirection(pixelIdx);
    ray.TMin = 0.001;
    ray.TMax = 1000;

    Payload payload;
    payload.materialIdx = MATERIAL_IDX_INVALID;
    payload.flags = 0;

    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, GBUFFER_HITGROUP_PRIMARY, 0, 0, ray, payload);
    outputGuideBuffers(payload, ray);

    GbufferData outGbufferData;
    outGbufferData.hitInfo = payload.hitInfo;
    outGbufferData.materialIdx = payload.materialIdx;
    outGbufferData.payloadFlags = payload.flags;
    outGbufferData.pad0 = outGbufferData.pad1 = 0;
    gbuffer[linearPixelIdx] = outGbufferData;
}
