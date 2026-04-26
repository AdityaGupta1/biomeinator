// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_structs.h"
#include "../rendering/common/common_registers.h"

#include "common/payload.hlsli"
#include "util/sampling.hlsli"

StructuredBuffer<Material> materials : REGISTER_T(RT, MATERIALS);

SamplerState texSampler : REGISTER_S(RT, TEX_SAMPLER);

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

float4 getMaterialBaseColor(const Material material, const float2 uv, const float mipLevel)
{
    float4 baseColor = float4(material.baseColor, 1.f);
    if (material.baseColorTextureId != TEXTURE_ID_INVALID)
    {
        Texture2D<float4> tex = ResourceDescriptorHeap[material.baseColorTextureId];
        baseColor = tex.SampleLevel(texSampler, uv, mipLevel);
    }
    return baseColor;
}

float3 getMaterialEmissiveColor(const Material material, const float2 uv, const float mipLevel)
{
    float3 emissiveColor = material.emissiveColor;
    if (material.emissiveColorTextureId != TEXTURE_ID_INVALID)
    {
        Texture2D<float4> tex = ResourceDescriptorHeap[material.emissiveColorTextureId];
        emissiveColor = tex.SampleLevel(texSampler, uv, mipLevel).rgb;
    }
    return emissiveColor * material.emissiveStrength;
}

// this is the recommended method from the DLSS-RR integration guide (https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS-RR%20Integration%20Guide.pdf)
// alpha = roughness^2
float3 calculateDlssSpecularAlbedo(const float3 glossyReflectionTint, const float alpha, float nDotV)
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
    bias *= saturate(glossyReflectionTint.g * 50);
    return mad(glossyReflectionTint, max(0, scale), max(0, bias));
}

float3 evaluateBsdf(
    const Material material,
    const float2 uv,
    const float3 wo_WS,
    const float3 wi_WS,
    const float3 surfNor_WS,
    const float mipLevel)
{
    if (!material.hasDiffuse() || dot(wi_WS, surfNor_WS) < 0.f) // TODO: revisit after adding roughness
    {
        return 0;
    }

    const float3 diffuseAlbedo = getMaterialBaseColor(material, uv, mipLevel).rgb;

    float fresnelReflectance = 0.f;
    if (material.hasGlossyReflection())
    {
        fresnelReflectance = walterFresnel(material.ior, cosTheta(wo_WS, surfNor_WS));
    }

    return diffuseAlbedo * M_INV_PI * (1.f - fresnelReflectance);
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
    const float mipLevel,
    inout RandomNumberGenerator rng)
{
    BsdfSample result;
    result.bsdfValue = float3(0, 0, 0);
    result.wasSpecular = false;

    if (!material.canScatter())
    {
        return result;
    }

    const bool hasGlossyReflection = material.hasGlossyReflection();
    const bool hasDiffuseOrGlossyTransmission = material.hasDiffuseOrGlossyTransmission();

    float fresnelReflectance;
    bool chooseReflect;
    if (hasGlossyReflection && !hasDiffuseOrGlossyTransmission)
    {
        fresnelReflectance = 1.f;
        chooseReflect = true;
    }
    else if (!hasGlossyReflection && hasDiffuseOrGlossyTransmission)
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
        result.wi_WS = normalize(reflect(-wo_WS, surfNor_WS));
        // pdf cancels out with the `* fresnelReflectance` in bsdfValue, so actual bsdf value is material.glossyReflectionTint * implicit fresnelReflectance from random chance of choosing reflection
        result.pdf = fresnelReflectance;
        result.bsdfValue = material.glossyReflectionTint * fresnelReflectance;
        result.wasSpecular = true;
    }
    else // diffuse or glossy transmission
    {
        const float oneMinusFresnelReflectance = 1.f - fresnelReflectance;

        if (material.hasGlossyTransmission()) // glossy transmission overrides diffuse
        {
            // ior parameter here is ratio of "from medium ior" over "to medium ior"
            // e.g. 1.f / 1.5f for going from air to glass
            result.wi_WS = normalize(refract(-wo_WS, surfNor_WS, 1.f / material.ior));
            result.pdf = oneMinusFresnelReflectance;
            result.bsdfValue = getMaterialBaseColor(material, uv, mipLevel).rgb * oneMinusFresnelReflectance;
            result.wasSpecular = true;
        }
        else
        {
            result.wi_WS = sampleHemisphereCosineWeighted(surfNor_WS, rng);
            result.pdf = absCosTheta(result.wi_WS, surfNor_WS) * oneMinusFresnelReflectance * M_INV_PI;
            result.bsdfValue = evaluateBsdf(material, uv, wo_WS, result.wi_WS, surfNor_WS, mipLevel);
        }
    }

    return result;
}

float bsdfPdf(
    const Material material,
    const float3 wo_WS,
    const float3 wi_WS,
    const float3 surfNor_WS)
{
    if (!material.hasDiffuse() || dot(wi_WS, surfNor_WS) < 0.f) // TODO: update this after adding roughness
    {
        return 0.f;
    }

    float pdf = hemisphereCosineWeightedPdf(wi_WS, surfNor_WS);

    if (material.hasGlossyReflection())
    {
        const float fresnelReflectance = walterFresnel(material.ior, cosTheta(wo_WS, surfNor_WS));
        pdf *= (1.f - fresnelReflectance);
    }

    return pdf;
}

Material getMaterialFromPayload(const Payload payload)
{
    Material material = materials[payload.materialIdx];

    if (bool(payload.flags & PAYLOAD_FLAG_BACKFACE_HIT))
    {
        material.ior = 1.f / material.ior;
    }

    return material;
}

// Attempts to split surfMaterial for path splitting. Returns true if the material was split, in which case surfMaterial
// is replaced with the split variant for pathSplitIdx and pathWeight is scaled accordingly. Returns false if no split
// applies; the caller should then early-out for pathSplitIdx == 1.
bool trySplitMaterial(inout Material surfMaterial,
                      const float2 uv,
                      const float3 surfNor_WS,
                      const float3 wo_WS,
                      const float mipLevel,
                      const uint pathSplitIdx,
                      inout float3 pathWeight)
{
    if (surfMaterial.hasDiffuse() && surfMaterial.baseColorTextureId != TEXTURE_ID_INVALID)
    {
        const float4 baseColorSample = getMaterialBaseColor(surfMaterial, uv, mipLevel);
        if (baseColorSample.a < 0.999f)
        {
            const float alpha = baseColorSample.a;
            if (pathSplitIdx == 0)
            {
                // opaque
                surfMaterial.baseColor = baseColorSample.rgb;
                surfMaterial.baseColorTextureId = TEXTURE_ID_INVALID;
                pathWeight *= alpha;
            }
            else
            {
                // transparent
                // TODO: use a special passthrough material type instead of co-opting specular transmission
                surfMaterial.flags = MATERIAL_FLAG_GLOSSY_TRANSMISSION;
                surfMaterial.baseColor = float3(1.f, 1.f, 1.f);
                surfMaterial.baseColorTextureId = TEXTURE_ID_INVALID;
                surfMaterial.glossyReflectionTint = float3(0.f, 0.f, 0.f);
                surfMaterial.ior = 1.f; // passthrough without refraction
                surfMaterial.emissiveStrength = 0.f;
                surfMaterial.emissiveColor = float3(0.f, 0.f, 0.f);
                surfMaterial.emissiveColorTextureId = TEXTURE_ID_INVALID;
                pathWeight *= (1.f - alpha);
            }
            return true;
        }
    }

    if (surfMaterial.hasGlossyReflection() && (surfMaterial.hasDiffuseOrGlossyTransmission() || surfMaterial.hasEmission()))
    {
        const float fresnelReflectance = walterFresnel(surfMaterial.ior, cosTheta(wo_WS, surfNor_WS));

        if (pathSplitIdx == 0)
        {
            // diffuse and transmission lobes, and emission
            surfMaterial.flags &= MATERIAL_FLAGS_DIFFUSE_OR_GLOSSY_TRANSMISSION;
            surfMaterial.glossyReflectionTint = float3(0, 0, 0);
            pathWeight *= (1.f - fresnelReflectance);
        }
        else
        {
            // glossy reflection lobes
            surfMaterial.flags &= MATERIAL_FLAG_GLOSSY_REFLECTION;
            surfMaterial.baseColor = float3(0, 0, 0);
            surfMaterial.baseColorTextureId = TEXTURE_ID_INVALID;
            surfMaterial.emissiveStrength = 0.f;
            surfMaterial.emissiveColor = float3(0, 0, 0);
            surfMaterial.emissiveColorTextureId = TEXTURE_ID_INVALID;
            pathWeight *= fresnelReflectance;
        }
        return true;
    }

    return false;
}
