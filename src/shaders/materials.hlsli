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

#pragma once

#include "../rendering/common/common_structs.h"
#include "../rendering/common/common_registers.h"

#include "util/math.hlsli"
#include "util/rng.hlsli"

StructuredBuffer<Material> materials : REGISTER_T(RT_REGISTER_MATERIALS, RT_REGISTER_SPACE);

SamplerState texSampler : REGISTER_S(RT_REGISTER_TEX_SAMPLER, RT_REGISTER_SPACE);

float3 sampleHemisphereCosineWeighted(const float3 normal_WS, inout RandomSampler rng)
{
    const float2 rndSample = rng.nextFloat2();
    const float r = sqrt(rndSample.x);
    const float theta = M_TWO_PI * rndSample.y;
    const float3 sampledDir_OS = float3(r * cos(theta), r * sin(theta), sqrt(1 - rndSample.x));
    return normalize(mul(computeTBN(normal_WS), sampledDir_OS));
}

// "An Inexpensive BRDF Model for Physically-based Rendering", Schlick, 1994
float schlickFresnel(const float eta, const float cosThetaWo)
{
    float R0 = (1.f - eta) / (1.f + eta);
    R0 = R0 * R0;
    return R0 + (1.f - R0) * pow(1.f - cosThetaWo, 5.f);
}

// "Microfacet Models for Refraction through Rough Surfaces", Walter et al., 2007
float walterFresnel(const float eta, const float cosThetaWo)
{
    const float c = cosThetaWo;
    float g = eta * eta - 1.f + c * c;

    if (g < 0.f) // total internal reflection
    {
        return 1.f;
    }

    g = sqrt(g);
    const float a = (g - c) / (g + c);
    const float b = (c * (g + c) - 1.f) / (c * (g - c) + 1.f);
    return 0.5f * a * a * (1 + b * b);
}

float3 getMaterialDiffuseAlbedo(const Material material, const float2 uv)
{
    float3 diffuseAlbedo = material.baseColor;
    if (material.baseColorTextureId != TEXTURE_ID_INVALID)
    {
        Texture2D<float4> tex = ResourceDescriptorHeap[material.baseColorTextureId];
        diffuseAlbedo = tex.SampleLevel(texSampler, uv, 0).rgb;
    }
    return diffuseAlbedo;
}

// this is the recommended method from the DLSS-RR integration guide (https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS-RR%20Integration%20Guide.pdf)
// alpha = roughness^2
float3 calculateDlssSpecularAlbedo(const float3 specularColor, const float alpha, float nDotV)
{
    nDotV = abs(nDotV);
    // [Ray Tracing Gems, Chapter 32]
    float4 X;
    X.x = 1.f;
    X.y = nDotV;
    X.z = nDotV * nDotV;
    X.w = nDotV * X.z;
    float4 Y;
    Y.x = 1.f;
    Y.y = alpha;
    Y.z = alpha * alpha;
    Y.w = alpha * Y.z;
    const float2x2 M1 = float2x2(0.99044f, -1.28514f, 1.29678f, -0.755907f);
    const float3x3 M2 = float3x3(1.f, 2.92338f, 59.4188f, 20.3225f, -27.0302f, 222.592f, 121.563f, 626.13f, 316.627f);
    const float2x2 M3 = float2x2(0.0365463f, 3.32707f, 9.0632f, -9.04756f);
    const float3x3 M4 = float3x3(1.f, 3.59685f, -1.36772f, 9.04401f, -16.3174f, 9.22949f, 5.56589f, 19.7886f, -20.2123f);
    float bias = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw));
    const float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw));
    // This is a hack for specular reflectance of 0
    bias *= saturate(specularColor.g * 50);
    return mad(specularColor, max(0, scale), max(0, bias));
}

float3 evaluateBsdf(
    const Material material,
    const float2 uv,
    const float3 wo_WS,
    const float3 wi_WS,
    const float3 surfNor_WS,
    bool calculateFresnelReflectance,
    float fresnelReflectance = 0.f)
{
    if (material.hasDiffuse())
    {
        const float3 diffuseAlbedo = getMaterialDiffuseAlbedo(material, uv);

        if (calculateFresnelReflectance && material.hasSpecularReflection())
        {
            fresnelReflectance = walterFresnel(material.ior, cosTheta(wo_WS, surfNor_WS));
        }

        return diffuseAlbedo * M_INV_PI * (1.f - fresnelReflectance);
    }

    return float3(0, 0, 0);
}

struct BsdfSample
{
    float3 wi_WS;
    float pdf;
    float3 bsdfValue;
    bool wasSpecular;
};

BsdfSample sampleBsdf(
    const Material material,
    const float2 uv,
    const float3 wo_WS,
    const float3 surfNor_WS,
    inout RandomSampler rng)
{
    BsdfSample result;
    result.bsdfValue = float3(0, 0, 0);
    result.wasSpecular = false;

    const bool canReflect = material.canReflect();
    const bool canTransmit = material.canTransmit();

    if (!canReflect && !canTransmit)
    {
        return result;
    }

    float fresnelReflectance;
    bool chooseReflect;
    if (canReflect && !canTransmit)
    {
        fresnelReflectance = 1.f;
        chooseReflect = true;
    }
    else if (!canReflect && canTransmit)
    {
        fresnelReflectance = 0.f;
        chooseReflect = false;
    }
    else
    {
        fresnelReflectance = walterFresnel(material.ior, cosTheta(wo_WS, surfNor_WS));
        chooseReflect = (rng.nextFloat() < fresnelReflectance);
    }

    if (chooseReflect)
    {
        const float3 wi_WS = normalize(reflect(-wo_WS, surfNor_WS));
        result.wi_WS = wi_WS;
        result.pdf = fresnelReflectance;
        result.bsdfValue = material.specularColor * fresnelReflectance;
        result.wasSpecular = true;
    }
    else
    {
        const float3 wi_WS = sampleHemisphereCosineWeighted(surfNor_WS, rng);
        result.wi_WS = wi_WS;
        result.pdf = absCosTheta(wi_WS, surfNor_WS) * (1.f - fresnelReflectance) * M_INV_PI;
        const float3 bsdfValue = evaluateBsdf(material, uv, wo_WS, wi_WS, surfNor_WS, false /*calculateFresnelReflectance*/, fresnelReflectance);
        result.bsdfValue = bsdfValue;
    }

    return result;
}

float bsdfPdf(
    const Material material,
    const float3 wo_WS,
    const float3 wi_WS,
    const float3 surfNor_WS)
{
    if (!material.canScatter())
    {
        return 0.f;
    }

    const bool canReflect = material.canReflect();
    const bool canTransmit = material.canTransmit();

    if (!canTransmit)
    {
        return 0.f; // TODO: update this after adding microfacet reflection
    }

    float pdf = absCosTheta(wi_WS, surfNor_WS) * M_INV_PI;

    if (canReflect)
    {
        const float fresnelReflectance = walterFresnel(material.ior, cosTheta(wo_WS, surfNor_WS));
        pdf *= (1.f - fresnelReflectance);
    }

    return pdf;
}

bool shouldSplitMaterial(const Material material)
{
    return material.canReflect() && (material.canTransmit() || material.hasEmission());
}

Material getSplitMaterial(const Material material, const float3 surfNor_WS, const float3 wo_WS, const uint pathSplitIdx, inout float3 pathWeight)
{
    const float fresnelReflectance = walterFresnel(material.ior, cosTheta(wo_WS, surfNor_WS));

    Material splitMaterial;
    splitMaterial.flags = 0;
    splitMaterial.baseColor = float3(0, 0, 0);
    splitMaterial.baseColorTextureId = TEXTURE_ID_INVALID;
    splitMaterial.specularColor = float3(0, 0, 0);
    splitMaterial.ior = 1.f;
    splitMaterial.emissiveStrength = 0.f;
    splitMaterial.emissiveColor = float3(0, 0, 0);

    if (pathSplitIdx == 0)
    {
        // "Transmit" lobes and emission
        splitMaterial.flags = material.flags & MATERIAL_FLAGS_TRANSMIT;
        splitMaterial.baseColor = material.baseColor;
        splitMaterial.baseColorTextureId = material.baseColorTextureId;
        splitMaterial.emissiveStrength = material.emissiveStrength;
        splitMaterial.emissiveColor = material.emissiveColor;
        pathWeight *= (1.f - fresnelReflectance);
    }
    else
    {
        // "Reflect" lobes
        splitMaterial.flags = material.flags & MATERIAL_FLAGS_REFLECT;
        splitMaterial.specularColor = material.specularColor;
        splitMaterial.ior = material.ior;
        pathWeight *= fresnelReflectance;
    }

    return splitMaterial;
}
