// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

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
StructuredBuffer<VertexTangent> tangents : REGISTER_T(RT, TANGENTS);
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

uint3 getTriangleVertexIndices(const InstanceData instanceData, const uint triIdx)
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

    return uint3(i0, i1, i2);
}

void loadVertsFromInstance(const InstanceData instanceData, const uint triIdx, out Vertex v0, out Vertex v1, out Vertex v2)
{
    const uint3 indices = getTriangleVertexIndices(instanceData, triIdx);
    v0 = verts[instanceData.vertsBufferOffset + indices.x];
    v1 = verts[instanceData.vertsBufferOffset + indices.y];
    v2 = verts[instanceData.vertsBufferOffset + indices.z];
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

float2 getUvAtHit(const InstanceData instanceData, const uint triIdx, const float2 bary2)
{
    Vertex v0, v1, v2;
    loadVertsFromInstance(instanceData, triIdx, v0, v1, v2);

    const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);
    return v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
}

float4 getMaterialBaseColorAtHit(const Material material, const InstanceData instanceData,
    const PerTriangleData perTriData, const uint triIdx, const float2 bary2, const float mipLevel)
{
    const float2 uv = getUvAtHit(instanceData, triIdx, bary2);

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

    Material material = materials[materialIdx];
    if (bool(payload.flags & PAYLOAD_FLAG_REFRACTION_PASSTHROUGH) && material.hasGlossyTransmission() &&
        material.roughnessTextureId != TEXTURE_ID_INVALID && !material.hasPackedAux())
    {
        // A roughness map can contain perfectly specular texels. Shadow passthrough must
        // classify the same resolved surface as the path tracer, not just its scalar factor.
        const float width = getRayConeWidthAtDistance(payload.rayCone, rayT);
        const PerTriangleData data = perTriDatas[instanceData.perTriDatasBufferOffset + primitiveIdx];
        material.roughness = getMaterialRoughness(material, getUvAtHit(instanceData, primitiveIdx, barycentrics),
            makeUntintedTexSampleCtx(computeMipLevel(width), data.texArraySliceIdx));
    }
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

[shader("closesthit")]
void ClosestHit_Primary(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    const InstanceData instanceData = instanceDatas[InstanceID()];
    const uint materialIdx = instanceData.materialIdx;

    const uint3 vertexIndices = getTriangleVertexIndices(instanceData, PrimitiveIndex());
    const Vertex v0 = verts[instanceData.vertsBufferOffset + vertexIndices.x];
    const Vertex v1 = verts[instanceData.vertsBufferOffset + vertexIndices.y];
    const Vertex v2 = verts[instanceData.vertsBufferOffset + vertexIndices.z];

    const float2 bary2 = attribs.barycentrics;
    const float3 bary = float3(1 - bary2.x - bary2.y, bary2.xy);

    const float3 hitPos_OS = v0.pos_OS * bary.x + v1.pos_OS * bary.y + v2.pos_OS * bary.z;
    payload.hitInfo.hitPos_WS = mul(float4(hitPos_OS, 1.f), ObjectToWorld4x3()).xyz;

    const float3 hitShadingNor_OS = octDecode(v0.packedNor) * bary.x + octDecode(v1.packedNor) * bary.y + octDecode(v2.packedNor) * bary.z;
    float3 shadingNor_WS = normalize(mul(hitShadingNor_OS, (float3x3) WorldToObject3x4()));
    // Geometric normal, oriented to agree with the interpolated normal so no winding convention is assumed
    float3 geoNor_WS = normalize(mul(cross(v1.pos_OS - v0.pos_OS, v2.pos_OS - v0.pos_OS), (float3x3) WorldToObject3x4()));
    if (dot(geoNor_WS, shadingNor_WS) < 0.f)
    {
        geoNor_WS = -geoNor_WS;
    }

    const Material material = materials[materialIdx];

    payload.hitInfo.uv = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
    const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + PrimitiveIndex()];
    const bool hasNormalMap = materialIdx != MATERIAL_IDX_INVALID && material.normalTextureId != TEXTURE_ID_INVALID &&
                              (!material.hasPackedAux() || bool(perTriData.flags & TRIANGLE_FLAG_NORMAL_MAP));
    const bool hasGlossy = materialIdx != MATERIAL_IDX_INVALID &&
                           (material.hasGlossy() || (hasNormalMap && bool(perTriData.flags & TRIANGLE_FLAG_IS_GLASS)));
    const bool isWaterTop = bool(perTriData.flags & TRIANGLE_FLAG_IS_WATER_TOP);

    // Orient the base surface before perturbing it. A mapped normal facing away from
    // the ray must not be flipped into the solid at grazing angles.
    const float3 frameShadingNor_WS = shadingNor_WS;
    const float3 wo_WS = -WorldRayDirection();
    // Classify backfaces using the geometric normal: interpolated normals can face away at grazing
    // angles on coarse meshes, and using them here would incorrectly invert the IOR.
    if (dot(geoNor_WS, wo_WS) < 0.f)
    {
        geoNor_WS = -geoNor_WS;
        shadingNor_WS = -shadingNor_WS;
        payload.flags |= PAYLOAD_FLAG_BACKFACE_HIT;
    }
    // Retain the interpolated normal when it faces away from the ray (Cycles-style handling; see #371).
    // Opaque geometric-backside directions are rejected by continuation and direct-light sampling.

    if (hasNormalMap)
    {
        float3 tangent_WS;
        float tangentSign;
        if (instanceData.tangentsBufferOffset != TANGENT_BUFFER_OFFSET_INVALID)
        {
            const VertexTangent t0 = tangents[instanceData.tangentsBufferOffset + vertexIndices.x];
            const VertexTangent t1 = tangents[instanceData.tangentsBufferOffset + vertexIndices.y];
            const VertexTangent t2 = tangents[instanceData.tangentsBufferOffset + vertexIndices.z];
            const float3 tangent_OS = octDecode(t0.packedTangent) * bary.x +
                                     octDecode(t1.packedTangent) * bary.y + octDecode(t2.packedTangent) * bary.z;
            tangent_WS = mul(tangent_OS, (float3x3) ObjectToWorld4x3());
            tangentSign = t0.handedness * (determinant((float3x3) ObjectToWorld4x3()) < 0.f ? -1.f : 1.f);
        }
        else
        {
            // Terrain derives its frame from face UVs without stored tangent attributes, so block rotations
            // and differently oriented faces rotate the normal texture with them.
            const float3 e1 = mul(v1.pos_OS - v0.pos_OS, (float3x3) ObjectToWorld4x3());
            const float3 e2 = mul(v2.pos_OS - v0.pos_OS, (float3x3) ObjectToWorld4x3());
            const float2 duv1 = v1.uv - v0.uv, duv2 = v2.uv - v0.uv;
            const float det = duv1.x * duv2.y - duv1.y * duv2.x;
            tangent_WS = abs(det) > 1e-10f ? (e1 * duv2.y - e2 * duv1.y) / det : float3(0.f, 0.f, 0.f);
            const float3 uvBitangent_WS = abs(det) > 1e-10f ? (e2 * duv1.x - e1 * duv2.x) / det : float3(0.f, 0.f, 0.f);
            tangentSign = dot(cross(frameShadingNor_WS, tangent_WS), uvBitangent_WS) < 0.f ? -1.f : 1.f;
        }

        tangent_WS -= frameShadingNor_WS * dot(frameShadingNor_WS, tangent_WS);

        if (dot(tangent_WS, tangent_WS) > 1e-12f)
        {
            tangent_WS = normalize(tangent_WS);
            const float3 bitangent_WS = tangentSign * cross(frameShadingNor_WS, tangent_WS);
            const float coneWidth = getRayConeWidthAtDistance(payload.rayCone, RayTCurrent());
            const TexSampleCtx ctx = makeUntintedTexSampleCtx(computeMipLevel(coneWidth), perTriData.texArraySliceIdx);
            float3 n = 2.f * sampleTexture(material.hasArrayTexture(), material.normalTextureId, payload.hitInfo.uv, ctx).xyz - 1.f;
            n.xy *= material.normalScale;
            if (dot(n, n) > 1e-12f)
            {
                // Flip the entire authored frame with the base normal, preserving backface UV orientation.
                const float frameSign = dot(shadingNor_WS, frameShadingNor_WS) < 0.f ? -1.f : 1.f;
                shadingNor_WS = frameSign * normalize(n.x * tangent_WS + n.y * bitangent_WS + n.z * frameShadingNor_WS);
                // Keep mapped normals in the actual surface's hemisphere, including on smooth meshes.
                const float geoCos = dot(shadingNor_WS, geoNor_WS);
                if (geoCos < 1e-4f)
                {
                    shadingNor_WS = normalize(shadingNor_WS + (1e-4f - geoCos) * geoNor_WS);
                }
            }
        }
    }

    if (isWaterTop)
    {
        const float2 posXZ_WS = payload.hitInfo.hitPos_WS.xz + float2(cameraParams.globalInstanceOffset.xz);
        shadingNor_WS = waveShadingNormal(posXZ_WS, renderParams.animTime, WorldRayDirection(),
                                   bool(payload.flags & PAYLOAD_FLAG_BACKFACE_HIT));
    }
    else
    {
        // Glossy lobes need reflections to stay above the geometric surface. Use Cycles' bump-map
        // correction (ensure_valid_specular_reflection; see util/shading_normal.hlsli).
        if (hasGlossy)
        {
            shadingNor_WS = ensureValidSpecularReflection(geoNor_WS, wo_WS, shadingNor_WS);
        }
    }
    payload.hitInfo.hitShadingNor_WS = shadingNor_WS;
    payload.hitInfo.packedGeoNor = octEncode(geoNor_WS);

    payload.hitInfo.instanceId = InstanceID();
    payload.hitInfo.triangleIdx = PrimitiveIndex();

    payload.materialIdx = materialIdx;

    payload.flags |= PAYLOAD_FLAG_DID_HIT;
}

[shader("miss")]
void Miss(inout Payload payload)
{
    payload.flags &= ~PAYLOAD_FLAG_DID_HIT;
}

// Occlusion test for a shadow ray, which only ever needs the anyhit's candidate handling. The
// traversal runs inline in the caller, so there is no payload marshalling or shader-table dispatch
// per non-opaque candidate (measured -8..-16% path tracing vs. an accept-first-hit TraceRay,
// 2026-09). The payload never goes through TraceRay here; it is just the struct the candidate
// handling reads and updates (passthrough tint, water entry/exit T, ray cone, rng).
bool isSegmentOccluded(const RayDesc ray, inout Payload payload)
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
