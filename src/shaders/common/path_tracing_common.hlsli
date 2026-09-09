// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_hitgroups.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_structs.h"

#include "common/biome_map.hlsli"
#include "common/global_params.hlsli"
#include "common/procedural_color.hlsli"
#include "common/payload.hlsli"
#include "common/water_waves.hlsli"
#include "materials/materials.hlsli"
#include "util/packing.hlsli"
#include "util/ray.hlsli"
#include "util/shading_normal.hlsli"

RaytracingAccelerationStructure raytracingAcs : REGISTER_T(RT, RAYTRACING_ACS);

StructuredBuffer<InstanceData> instanceDatas : REGISTER_T(RT, INSTANCE_DATAS);
StructuredBuffer<PerTriangleData> perTriDatas : REGISTER_T(RT, PER_TRI_DATAS);

StructuredBuffer<Vertex> verts : REGISTER_T(RT, VERTS);
ByteAddressBuffer idxs : REGISTER_T(RT, IDXS);

#include "materials/water.hlsli"

uint getPathSplitIdx()
{
    if (bool(renderParams.doPathSplitting))
    {
        return DispatchRaysIndex().x % 2;
    }
    else
    {
        return 0;
    }
}

uint2 getPixelIdx()
{
    const uint2 dispatchIdx = DispatchRaysIndex().xy;
    if (bool(renderParams.doPathSplitting))
    {
        return uint2(dispatchIdx.x / 2, dispatchIdx.y);
    }
    else
    {
        return dispatchIdx;
    }
}

bool isPixelOutOfBounds(int2 pixelIdx)
{
    return any(pixelIdx < int2(0, 0)) || any(pixelIdx >= renderParams.renderSize);
}

void loadVertsFromInstance(const InstanceData instanceData, const uint triIdx, out Vertex v0, out Vertex v1, out Vertex v2)
{
    uint i0, i1, i2;
    if (bool(instanceData.hasIdxs))
    {
        const uint idxsBufferByteOffset = instanceData.idxsBufferByteOffset + triIdx * 3 * 4;
        i0 = idxs.Load(idxsBufferByteOffset + 0);
        i1 = idxs.Load(idxsBufferByteOffset + 4);
        i2 = idxs.Load(idxsBufferByteOffset + 8);
    }
    else
    {
        i0 = triIdx * 3;
        i1 = i0 + 1;
        i2 = i0 + 2;
    }

    v0 = verts[instanceData.vertsBufferOffset + i0];
    v1 = verts[instanceData.vertsBufferOffset + i1];
    v2 = verts[instanceData.vertsBufferOffset + i2];
}

// Ctx for surface shading at a hit; samples the biome map and the procedural color ramp once here
// so all color reads for the hit share them (c.f. makeUntintedTexSampleCtx())
TexSampleCtx makeTintedTexSampleCtx(const PerTriangleData perTriData, const float rayConeWidth, const float3 pos_WS)
{
    TexSampleCtx texCtx;
    texCtx.mipLevel = computeMipLevel(rayConeWidth);
    texCtx.arraySliceIdx = perTriData.texArraySliceIdx;
    texCtx.biomeTint = getBiomeTint(perTriData.flags, pos_WS.xz);
    texCtx.proceduralColor = getProceduralColor(perTriData.flags, pos_WS);
    return texCtx;
}

// Resolve every primary/secondary hit before any lobe-dependent shading or guide logic.
// Roughness needs the footprint at the hit; biome/procedural color is sampled separately.
Material getHitMaterial(const Payload payload, const float coneWidth)
{
    const PerTriangleData data = perTriDatas[
        instanceDatas[payload.hitInfo.instanceId].perTriDatasBufferOffset + payload.hitInfo.triangleIdx];
    return getMaterialFromPayload(payload, data.flags,
        makeUntintedTexSampleCtx(computeMipLevel(coneWidth), data.texArraySliceIdx));
}

float4 getMaterialBaseColorAtHit(const Material material, const InstanceData instanceData,
    const PerTriangleData perTriData, const uint triIdx, const float2 bary2, const float mipLevel)
{
    Vertex v0, v1, v2;
    loadVertsFromInstance(instanceData, triIdx, v0, v1, v2);

    const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);
    const float2 uv = unpackUintToFloat2(v0.packedUv) * bary.x + unpackUintToFloat2(v1.packedUv) * bary.y +
                      unpackUintToFloat2(v2.packedUv) * bary.z;

    // Cutout alpha and passthrough absorption don't care about biome tint or the packed aux
    // adjustments, so skip the map sample and the aux texture sample
    const TexSampleCtx texCtx = makeUntintedTexSampleCtx(mipLevel, perTriData.texArraySliceIdx);
    return getMaterialBaseColorNoAux(material, uv, texCtx);
}

// Decides whether a non-opaque candidate hit is accepted, applying passthrough tint, water
// entry/exit tracking and the stochastic alpha cutout to the payload on the way. Shared by the
// anyhit shader and the inline shadow ray query so a segment is traced identically either way.
bool acceptHitCandidate(inout Payload payload,
                        const uint instanceId,
                        const uint primitiveIdx,
                        const float2 barycentrics,
                        const float rayT,
                        const bool isFrontFace)
{
    const InstanceData instanceData = instanceDatas[instanceId];

    const uint materialIdx = instanceData.materialIdx;
    if (materialIdx == MATERIAL_IDX_INVALID)
    {
        return true;
    }

    const Material material = materials[materialIdx];
    // Only specular transmission can be passed through without scattering; rough glass is a real bounce
    const bool testRefractionPassthrough =
        bool(payload.flags & PAYLOAD_FLAG_REFRACTION_PASSTHROUGH) && material.isDeltaTransmission();
    const bool testAlphaCutout =
        material.hasDiffuse() && material.baseColorTextureId != TEXTURE_ID_INVALID;

    if (!testRefractionPassthrough && !testAlphaCutout)
    {
        return true;
    }

    const float coneWidth = getRayConeWidthAtDistance(payload.rayCone, rayT);
    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + primitiveIdx];
    const float4 baseColor = getMaterialBaseColorAtHit(
        material, instanceData, perTriData, primitiveIdx, barycentrics, computeMipLevel(coneWidth));

    if (testRefractionPassthrough)
    {
        payload.pathWeight *= baseColor.rgb;

        if (bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER))
        {
            // Track the first water entry/exit T for absorption in computePassthroughAbsorption.
            // NOTE: tracks only one entry/exit; breaks down for multiple water bodies along the ray.
            if (isFrontFace)
            {
                payload.waterEntryT = min(payload.waterEntryT, rayT);
            }
            else
            {
                payload.waterExitT = min(payload.waterExitT, rayT);
            }
        }

        return false;
    }

    if (baseColor.a < 0.999f) // testAlphaCutout
    {
        if (baseColor.a == 0.f)
        {
            return false;
        }

        // If path splitting is enabled, we will split for fractional opacity, so we don't want to ignore those hits in the gbuffer pass.
        const bool checkFractionalOpacity =
            !bool(renderParams.doPathSplitting) || !bool(payload.flags & PAYLOAD_FLAG_IS_GBUFFER);
        if (checkFractionalOpacity && nextFloat(payload.rng) > baseColor.a)
        {
            return false;
        }
    }

    return true;
}

[shader("anyhit")]
void AnyHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    const bool isFrontFace = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE;
    if (!acceptHitCandidate(payload, InstanceID(), PrimitiveIndex(), attribs.barycentrics, RayTCurrent(), isFrontFace))
    {
        IgnoreHit();
    }
}

// Finishes a triangle hit from its interpolated world-space attributes: orients the geometric and
// shading normals for a ray arriving from `wo_WS`, applies the water wave normal and the glossy
// reflection fix, and reports whether the hit is a backface. Shared by ClosestHit_Primary and
// rebuildHit so a vertex rebuilt from barycentrics matches the original hit exactly.
void finishHit(const InstanceData instanceData,
               const uint triangleIdx,
               const float3 wo_WS,
               const float3 rayDir_WS,
               const float3 geoNorUnoriented_WS,
               inout HitInfo hitInfo,
               out bool isBackface)
{
    float3 nor_WS = hitInfo.hitNor_WS;
    // Geometric normal, oriented to agree with the interpolated normal so no winding convention is assumed
    float3 geoNor_WS = geoNorUnoriented_WS;
    if (dot(geoNor_WS, nor_WS) < 0.f)
    {
        geoNor_WS = -geoNor_WS;
    }

    // Which side of the surface the ray is on is decided by the geometric normal: the interpolated normal can
    // face away from the ray on grazing hits of coarse meshes, and treating those as backfaces would invert the
    // IOR for them
    isBackface = dot(geoNor_WS, wo_WS) < 0.f;
    if (isBackface)
    {
        geoNor_WS = -geoNor_WS;
        nor_WS = -nor_WS;
    }

    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + triangleIdx];
    if (bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER_TOP))
    {
        const float2 posXZ_WS = hitInfo.hitPos_WS.xz + float2(cameraParams.globalInstanceOffset.xz);
        nor_WS = waveShadingNormal(posXZ_WS, renderParams.animTime, rayDir_WS, isBackface);
    }
    else
    {
        // Glossy lobes need a shading normal whose reflections stay above the surface (as Cycles' bump map
        // correction ensures); other materials keep the plain interpolated normal, facing the ray
        const uint materialIdx = instanceData.materialIdx;
        const bool hasGlossy = materialIdx != MATERIAL_IDX_INVALID && materials[materialIdx].hasGlossy();
        if (hasGlossy)
        {
            nor_WS = ensureValidSpecularReflection(geoNor_WS, wo_WS, nor_WS);
        }
        else if (dot(nor_WS, wo_WS) < 0.f)
        {
            // TODO: mirror Cycles instead, keeping the normal and killing samples that go under the geometric normal (see #371)
            nor_WS = -nor_WS;
        }
    }
    hitInfo.hitNor_WS = nor_WS;
}

float3x3 inverse3x3(const float3x3 m)
{
    const float3 c0 = cross(m[1], m[2]);
    const float3 c1 = cross(m[2], m[0]);
    const float3 c2 = cross(m[0], m[1]);
    const float det = dot(m[0], c0);
    // Rows of the inverse are the columns of the cofactor matrix over the determinant
    return transpose(float3x3(c0, c1, c2)) / det;
}

// Rebuilds a hit stored as instance/triangle/barycentrics on the current mesh, as seen from `fromPos_WS`,
// reproducing what ClosestHit_Primary would have produced. `geoNor_WS` is the triangle's unoriented
// geometric normal, as light sampling uses for light points. False if the instance id has been
// recycled since the hit was stored (generation mismatch).
bool rebuildHit(const uint instanceId,
                const uint generation,
                const uint triangleIdx,
                const float2 bary2,
                const float3 fromPos_WS,
                out HitInfo hitInfo,
                out float3 geoNor_WS,
                out bool isBackface)
{
    hitInfo.hitPos_WS = 0.f;
    hitInfo.instanceId = instanceId;
    hitInfo.hitNor_WS = 0.f;
    hitInfo.triangleIdx = triangleIdx;
    hitInfo.uv = 0.f;
    hitInfo.barycentrics = bary2;
    geoNor_WS = 0.f;
    isBackface = false;

    const InstanceData instanceData = instanceDatas[instanceId];
    if (instanceData.generation != generation)
    {
        return false;
    }

    Vertex v0, v1, v2;
    loadVertsFromInstance(instanceData, triangleIdx, v0, v1, v2);
    const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);

    const float3 hitPos_OS = v0.pos_OS * bary.x + v1.pos_OS * bary.y + v2.pos_OS * bary.z;
    hitInfo.hitPos_WS = mul(instanceData.objectToWorld, float4(hitPos_OS, 1.f)) +
                        float3(instanceData.transformOffset - cameraParams.globalInstanceOffset);
    const float3x3 objectToWorld3x3 = (float3x3)instanceData.objectToWorld;

    // Normals transform by the inverse transpose, which the row-vector product with the inverse gives
    const float3x3 worldToObject3x3 = inverse3x3(objectToWorld3x3);
    const float3 hitNor_OS = octDecode(v0.packedNor) * bary.x + octDecode(v1.packedNor) * bary.y + octDecode(v2.packedNor) * bary.z;
    hitInfo.hitNor_WS = normalize(mul(hitNor_OS, worldToObject3x3));
    geoNor_WS = normalize(mul(cross(v1.pos_OS - v0.pos_OS, v2.pos_OS - v0.pos_OS), worldToObject3x3));

    hitInfo.uv = unpackUintToFloat2(v0.packedUv) * bary.x + unpackUintToFloat2(v1.packedUv) * bary.y +
                 unpackUintToFloat2(v2.packedUv) * bary.z;

    const float3 rayDir_WS = normalize(hitInfo.hitPos_WS - fromPos_WS);
    finishHit(instanceData, triangleIdx, -rayDir_WS, rayDir_WS, geoNor_WS, hitInfo, isBackface);
    return true;
}

[shader("closesthit")]
void ClosestHit_Primary(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    const InstanceData instanceData = instanceDatas[InstanceID()];

    Vertex v0, v1, v2;
    loadVertsFromInstance(instanceData, PrimitiveIndex(), v0, v1, v2);

    const float2 bary2 = attribs.barycentrics;
    const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);

    const float3 hitPos_OS = v0.pos_OS * bary.x + v1.pos_OS * bary.y + v2.pos_OS * bary.z;
    payload.hitInfo.hitPos_WS = mul(float4(hitPos_OS, 1.f), ObjectToWorld4x3()).xyz;

    const float3 hitNor_OS = octDecode(v0.packedNor) * bary.x + octDecode(v1.packedNor) * bary.y + octDecode(v2.packedNor) * bary.z;
    payload.hitInfo.hitNor_WS = normalize(mul(hitNor_OS, (float3x3) WorldToObject3x4()));
    const float3 geoNor_WS = normalize(mul(cross(v1.pos_OS - v0.pos_OS, v2.pos_OS - v0.pos_OS), (float3x3) WorldToObject3x4()));

    payload.hitInfo.uv = unpackUintToFloat2(v0.packedUv) * bary.x + unpackUintToFloat2(v1.packedUv) * bary.y +
                         unpackUintToFloat2(v2.packedUv) * bary.z;
    payload.hitInfo.instanceId = InstanceID();
    payload.hitInfo.triangleIdx = PrimitiveIndex();
    payload.hitInfo.barycentrics = bary2;

    bool isBackface;
    finishHit(instanceData, PrimitiveIndex(), -WorldRayDirection(), WorldRayDirection(), geoNor_WS, payload.hitInfo, isBackface);
    if (isBackface)
    {
        payload.flags |= PAYLOAD_FLAG_BACKFACE_HIT;
    }

    payload.materialIdx = instanceData.materialIdx;

    payload.flags |= PAYLOAD_FLAG_DID_HIT;
}

[shader("miss")]
void Miss(inout Payload payload)
{
    payload.flags &= ~PAYLOAD_FLAG_DID_HIT;
}

// Occlusion test for a shadow ray, which only ever needs the anyhit's candidate handling. With
// `useRayQuery` the traversal runs inline in the caller, so there is no payload marshalling or
// shader-table dispatch per non-opaque candidate (measured -8..-16% path tracing vs. an
// accept-first-hit TraceRay, 2026-09); the payload is then just the struct the candidate handling
// reads and updates (passthrough tint, water entry/exit T, ray cone, rng). Otherwise it is an
// accept-first-hit TraceRay through the anyhit-only hit group, which the ReSTIR reuse passes use:
// inline traversal there gives up their SER ordering and costs registers.
bool isSegmentOccluded(const RayDesc ray, inout Payload payload, const bool useRayQuery)
{
    if (useRayQuery)
    {
        // The OMM opt-in is required because traversal over OMM-linked terrain is otherwise undefined.
        // SKIP_PROCEDURAL_PRIMITIVES means every candidate is a non-opaque triangle, exactly the
        // geometry the anyhit shader runs on; opaque hits commit in hardware and end the search.
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
                 RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS> query;
        query.TraceRayInline(raytracingAcs, RAY_FLAG_NONE, 0xFF, ray);
        while (query.Proceed())
        {
            if (acceptHitCandidate(payload, query.CandidateInstanceID(), query.CandidatePrimitiveIndex(),
                    query.CandidateTriangleBarycentrics(), query.CandidateTriangleRayT(), query.CandidateTriangleFrontFace()))
            {
                query.CommitNonOpaqueTriangleHit();
            }
        }
        return query.CommittedStatus() != COMMITTED_NOTHING;
    }

    // Only the miss shader clears DID_HIT
    payload.flags |= PAYLOAD_FLAG_DID_HIT;
    const uint rayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER;
    TraceRay(raytracingAcs, rayFlags, 0xFF, HITGROUP_LIGHTS, 0, 0, ray, payload);
    return bool(payload.flags & PAYLOAD_FLAG_DID_HIT);
}
