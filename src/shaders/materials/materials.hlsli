// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_structs.h"
#include "../rendering/common/common_registers.h"

#include "common/payload.hlsli"
#include "util/ggx.hlsli"
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

// arraySliceIdx only read when sampled tex is Texture2DArray; non-array callers may pass 0.
struct TexSampleCtx
{
    float mipLevel;
    uint arraySliceIdx;
    float4 biomeTint; // rgb = biome map tint, a = 1 to apply it (see getBiomeTint)
};

// Ctx for samples that don't apply the biome tint; tinted hits build the ctx with getBiomeTint instead
// (c.f. makeTintedTexSampleCtx())
TexSampleCtx makeUntintedTexSampleCtx(const float mipLevel, const uint arraySliceIdx)
{
    TexSampleCtx texCtx;
    texCtx.mipLevel = mipLevel;
    texCtx.arraySliceIdx = arraySliceIdx;
    texCtx.biomeTint = float4(1.f, 1.f, 1.f, 0.f);
    return texCtx;
}

float4 sampleTexture(const bool isArrayTexture, const uint texId, const float2 uv, const TexSampleCtx texCtx)
{
    if (isArrayTexture)
    {
        Texture2DArray<float4> tex = ResourceDescriptorHeap[texId];
        return tex.SampleLevel(texSampler, float3(uv, texCtx.arraySliceIdx), texCtx.mipLevel);
    }
    Texture2D<float4> tex = ResourceDescriptorHeap[texId];
    return tex.SampleLevel(texSampler, uv, texCtx.mipLevel);
}

// Base color without the packed-aux emission/tint adjustments; aux never affects alpha, so
// cutout and passthrough tests can use this cheaper path.
float4 getMaterialBaseColorNoAux(const Material material, const float2 uv, const TexSampleCtx texCtx)
{
    if (material.baseColorTextureId == TEXTURE_ID_INVALID)
    {
        return float4(material.baseColor, 1.f);
    }
    return sampleTexture(material.hasArrayTexture(), material.baseColorTextureId, uv, texCtx);
}

float4 getMaterialBaseColor(const Material material, const float2 uv, const TexSampleCtx texCtx)
{
    float4 baseColor = getMaterialBaseColorNoAux(material, uv, texCtx);
    if (material.hasPackedAux() && material.baseColorTextureId != TEXTURE_ID_INVALID
        && material.auxTextureId != TEXTURE_ID_INVALID)
    {
        const float4 aux = sampleTexture(material.hasArrayTexture(), material.auxTextureId, uv, texCtx);
        if (aux.r > 0.f) // emissive texels carry emission color, not diffuse
        {
            baseColor.rgb = 0.f;
        }
        // Tint-masked texels are authored grayscale; the biome tint provides the hue
        baseColor.rgb *= lerp(float3(1.f, 1.f, 1.f), texCtx.biomeTint.rgb, texCtx.biomeTint.a * aux.g);
    }
    return baseColor;
}

float3 getMaterialEmissiveColor(const Material material, const float2 uv, const TexSampleCtx texCtx)
{
    if (material.hasPackedAux())
    {
        // Emission color lives in the base color texture; aux.r is the per-texel strength.
        if (material.auxTextureId == TEXTURE_ID_INVALID || material.baseColorTextureId == TEXTURE_ID_INVALID)
        {
            return float3(0.f, 0.f, 0.f);
        }
        const float auxStrength = sampleTexture(material.hasArrayTexture(), material.auxTextureId, uv, texCtx).r;
        if (auxStrength <= 0.f) // almost all texels; skip the base color sample for them
        {
            return float3(0.f, 0.f, 0.f);
        }
        const float3 emissiveColor = sampleTexture(material.hasArrayTexture(), material.baseColorTextureId, uv, texCtx).rgb;
        return emissiveColor * auxStrength * material.emissiveStrength;
    }

    const float3 emissiveColor = (material.auxTextureId == TEXTURE_ID_INVALID)
        ? material.emissiveColor
        : sampleTexture(material.hasArrayTexture(), material.auxTextureId, uv, texCtx).rgb;
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

// Probability of choosing the glossy reflection lobe in sampleBsdf; also the Fresnel weight applied
// to that lobe, so it cancels out of the sampling weight. evaluateBsdf and bsdfPdf must use the exact
// same value or MIS breaks silently.
float glossyReflectionProbability(const Material material, const float3 wo_WS, const float3 surfNor_WS)
{
    if (!material.hasGlossyReflection())
    {
        return 0.f;
    }
    if (!material.hasDiffuseOrGlossyTransmission())
    {
        return 1.f;
    }
    return walterFresnel(material.ior, cosTheta(wo_WS, surfNor_WS));
}

float3 evaluateBsdf(
    const Material material,
    const float2 uv,
    const float3 wo_WS,
    const float3 wi_WS,
    const float3 surfNor_WS,
    const TexSampleCtx texCtx)
{
    const bool isTransmission = dot(wi_WS, surfNor_WS) < 0.f;
    const float fresnelReflectance = glossyReflectionProbability(material, wo_WS, surfNor_WS);

    float3 bsdf = 0;

    if (material.hasDiffuse())
    {
        const float3 diffuseAlbedo = getMaterialBaseColor(material, uv, texCtx).rgb;
        if (isTransmission)
        {
            // Thin-wall diffuse transmission; no Fresnel term on the back side
            bsdf += diffuseAlbedo * M_INV_PI * material.diffuseTransmission;
        }
        else
        {
            bsdf += diffuseAlbedo * M_INV_PI * (1.f - fresnelReflectance) * (1.f - material.diffuseTransmission);
        }
    }

    // The specular lobe (roughness = 0) is a delta distribution and can't be evaluated for arbitrary directions
    if (material.hasGlossyReflection() && material.roughness > 0.f && !isTransmission)
    {
        const float cosThetaWo = cosTheta(wo_WS, surfNor_WS);
        const float cosThetaWi = cosTheta(wi_WS, surfNor_WS);
        if (cosThetaWo > 0.f && cosThetaWi > 0.f)
        {
            const float alpha = material.roughness * material.roughness;
            const float3 h_WS = normalize(wo_WS + wi_WS);
            const float d = ggxDistribution(alpha, cosTheta(h_WS, surfNor_WS));
            const float g = ggxSmithG2(alpha, cosThetaWo, cosThetaWi);
            // The glossy lobe matches Cycles' Glossy BSDF node (Multiscatter GGX): constant Fresnel
            // with the tint as the single-scattering albedo. Any dielectric Fresnel weighting is
            // applied outside the lobe via fresnelReflectance, mirroring the Blender node group's
            // Fresnel-node mix.
            const float3 msCompensation =
                ggxEnergyCompensation(material.roughness, cosThetaWo, material.glossyReflectionTint);
            bsdf += material.glossyReflectionTint * fresnelReflectance * msCompensation * d * g / (4.f * cosThetaWo * cosThetaWi);
        }
    }

    return bsdf;
}

float bsdfPdf(
    const Material material,
    const float3 wo_WS,
    const float3 wi_WS,
    const float3 surfNor_WS)
{
    const bool isTransmission = dot(wi_WS, surfNor_WS) < 0.f;
    const float fresnelReflectance = glossyReflectionProbability(material, wo_WS, surfNor_WS);

    float pdf = 0.f;

    if (material.hasDiffuse())
    {
        // Must mirror the hemisphere split in sampleBsdf exactly or MIS breaks silently
        const float diffusePdf = isTransmission
            ? hemisphereCosineWeightedPdf(wi_WS, -surfNor_WS) * material.diffuseTransmission
            : hemisphereCosineWeightedPdf(wi_WS, surfNor_WS) * (1.f - material.diffuseTransmission);
        pdf += diffusePdf * (1.f - fresnelReflectance);
    }

    if (material.hasGlossyReflection() && material.roughness > 0.f && !isTransmission)
    {
        const float alpha = material.roughness * material.roughness;
        pdf += fresnelReflectance * ggxVndfReflectionPdf(alpha, wo_WS, wi_WS, surfNor_WS);
    }

    return pdf;
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
    const TexSampleCtx texCtx,
    inout RandomNumberGenerator rng)
{
    BsdfSample result;
    result.bsdfValue = float3(0, 0, 0);
    result.wasSpecular = false;

    if (!material.canScatter())
    {
        return result;
    }

    const float fresnelReflectance = glossyReflectionProbability(material, wo_WS, surfNor_WS);
    bool chooseReflect;
    if (fresnelReflectance <= 0.f)
    {
        chooseReflect = false;
    }
    else if (fresnelReflectance >= 1.f)
    {
        chooseReflect = true;
    }
    else
    {
        chooseReflect = (rng.nextFloat() < fresnelReflectance);
    }

    if (chooseReflect && material.roughness == 0.f)
    {
        result.wi_WS = normalize(reflect(-wo_WS, surfNor_WS));
        // pdf cancels out with the `* fresnelReflectance` in bsdfValue, so actual bsdf value is material.glossyReflectionTint * implicit fresnelReflectance from random chance of choosing reflection
        result.pdf = fresnelReflectance;
        result.bsdfValue = material.glossyReflectionTint * fresnelReflectance;
        result.wasSpecular = true;
        return result;
    }

    if (chooseReflect)
    {
        const float alpha = material.roughness * material.roughness;
        const float3 h_WS = sampleGgxVndf(wo_WS, surfNor_WS, alpha, rng);
        result.wi_WS = normalize(reflect(-wo_WS, h_WS));
        if (cosTheta(result.wi_WS, surfNor_WS) <= 0.f)
        {
            result.pdf = 1.f; // sample fell below the horizon; the zero bsdfValue kills the path
            return result;
        }
    }
    else if (material.hasGlossyTransmission()) // glossy transmission overrides diffuse
    {
        const float oneMinusFresnelReflectance = 1.f - fresnelReflectance;
        // ior parameter here is ratio of "from medium ior" over "to medium ior"
        // e.g. 1.f / 1.5f for going from air to glass
        result.wi_WS = normalize(refract(-wo_WS, surfNor_WS, 1.f / material.ior));
        result.pdf = oneMinusFresnelReflectance;
        result.bsdfValue = getMaterialBaseColor(material, uv, texCtx).rgb * oneMinusFresnelReflectance;
        result.wasSpecular = true;
        return result;
    }
    else
    {
        // Diffuse transmission splits the diffuse lobe across both hemispheres; when diffuse is the
        // only non-delta lobe, either pick has bsdf * cos / pdf = albedo, so path weights stay noise-free.
        float3 lobeNor_WS = surfNor_WS;
        if (material.diffuseTransmission > 0.f && rng.nextFloat() < material.diffuseTransmission)
        {
            lobeNor_WS = -surfNor_WS;
        }
        result.wi_WS = sampleHemisphereCosineWeighted(lobeNor_WS, rng);
    }

    // Non-delta sample: use the full mixture bsdf and pdf over all lobes that could have produced
    // wi_WS, keeping the estimator consistent with the values NEE uses for MIS
    result.pdf = bsdfPdf(material, wo_WS, result.wi_WS, surfNor_WS);
    result.bsdfValue = evaluateBsdf(material, uv, wo_WS, result.wi_WS, surfNor_WS, texCtx);
    return result;
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
                      const TexSampleCtx texCtx,
                      const uint pathSplitIdx,
                      inout float3 pathWeight)
{
    if (surfMaterial.hasDiffuse() && surfMaterial.baseColorTextureId != TEXTURE_ID_INVALID)
    {
        const float4 baseColorSample = getMaterialBaseColor(surfMaterial, uv, texCtx);
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
                surfMaterial.roughness = 0.f;
                surfMaterial.ior = 1.f; // passthrough without refraction
                surfMaterial.emissiveStrength = 0.f;
                surfMaterial.emissiveColor = float3(0.f, 0.f, 0.f);
                surfMaterial.auxTextureId = TEXTURE_ID_INVALID;
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
            surfMaterial.auxTextureId = TEXTURE_ID_INVALID;
            pathWeight *= fresnelReflectance;
        }
        return true;
    }

    return false;
}
