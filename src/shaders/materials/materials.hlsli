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

// "Microfacet Models for Refraction through Rough Surfaces", Walter et al., 2007
float walterFresnel(const float eta, const float cosThetaWo)
{
    // Interpolated shading normals can push cosThetaWo negative at silhouettes; clamp to the
    // grazing limit (F = 1) so lobe weights stay in [0, 1]
    const float c = max(cosThetaWo, 0.f);
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
// to that lobe, so it cancels out of the sampling weight. sampleBsdf and evaluateBsdf must use the exact
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

// Terms shared by the value and pdf of the dielectric lobe (glossy reflection + glossy transmission, i.e. glass).
// Its two lobes share one microfacet normal, so unlike glossyReflectionProbability the Fresnel weight is
// evaluated per microfacet from the half vector that maps wo to wi, and the lobe that produced wi is
// identified by wi's hemisphere. Mirrors Cycles' bsdf_microfacet_eval for CLOSURE_BSDF_MICROFACET_GGX_GLASS_ID.
struct DielectricLobeTerms
{
    bool isValid;
    bool isTransmission;
    float fresnelReflectance;
    float cosThetaWo;
    float cosThetaWoH;
    float absCosThetaWi;
    float d;
    float g1Wo;
    float g2;
    float jacobian; // |d(omega_h) / d(omega_i)| for the lobe that produced wi
};

// A relative ior this close to 1 makes refraction a passthrough (wi = -wo), for which the refraction half vector
// degenerates; such dielectrics are treated as delta, like Cycles does
static const float DIELECTRIC_PASSTHROUGH_IOR_EPSILON = 1e-4f;

bool isDeltaDielectric(const Material material)
{
    return material.roughness == 0.f || abs(material.ior - 1.f) < DIELECTRIC_PASSTHROUGH_IOR_EPSILON;
}

DielectricLobeTerms dielectricLobeTerms(const Material material, const float3 wo_WS, const float3 wi_WS, const float3 surfNor_WS)
{
    DielectricLobeTerms terms;
    terms.isValid = false;
    terms.isTransmission = dot(wi_WS, surfNor_WS) < 0.f;
    terms.cosThetaWo = cosTheta(wo_WS, surfNor_WS);
    terms.absCosThetaWi = absCosTheta(wi_WS, surfNor_WS);
    // Delta lobes can't be evaluated for arbitrary directions
    if (isDeltaDielectric(material) || terms.cosThetaWo <= 0.f || terms.absCosThetaWi <= 0.f)
    {
        return terms;
    }

    float3 h_WS = terms.isTransmission ? refractionHalfVector(wo_WS, wi_WS, material.ior) : normalize(wo_WS + wi_WS);
    // D, G and Fresnel are symmetric in the half vector's sign, so keep it on wo's side (the refraction half
    // vector points to the lower-ior side, which is behind the surface when leaving the denser medium)
    if (dot(h_WS, surfNor_WS) < 0.f)
    {
        h_WS = -h_WS;
    }
    terms.cosThetaWoH = dot(wo_WS, h_WS);
    const float cosThetaWiH = dot(wi_WS, h_WS);
    // No microfacet maps wo to wi unless wo sees its front and a refracted wi leaves through its back; the
    // refraction half vector formula still yields a same-side h for directions no refraction can reach
    if (terms.cosThetaWoH <= 0.f || (terms.isTransmission && cosThetaWiH >= 0.f))
    {
        return terms;
    }

    const float alpha = material.roughness * material.roughness;
    terms.fresnelReflectance = material.hasGlossyReflection() ? walterFresnel(material.ior, terms.cosThetaWoH) : 0.f;
    terms.d = ggxDistribution(alpha, cosTheta(h_WS, surfNor_WS));
    terms.g1Wo = ggxSmithG1(alpha, terms.cosThetaWo);
    terms.g2 = ggxSmithG2(alpha, terms.cosThetaWo, terms.absCosThetaWi);
    terms.jacobian = terms.isTransmission
        ? refractionJacobian(material.ior, terms.cosThetaWoH, cosThetaWiH)
        : 1.f / (4.f * terms.cosThetaWoH);
    terms.isValid = true;
    return terms;
}

float dielectricLobeWeight(const DielectricLobeTerms terms)
{
    return terms.isTransmission ? (1.f - terms.fresnelReflectance) : terms.fresnelReflectance;
}

// pdf (in solid angle of wi) of sampling wi in sampleDielectricBsdf: VNDF density of the half vector times the
// probability of picking the lobe, mapped to wi through the lobe's Jacobian
float dielectricBsdfPdf(const DielectricLobeTerms terms)
{
    if (!terms.isValid)
    {
        return 0.f;
    }
    return dielectricLobeWeight(terms) * terms.g1Wo * terms.d * terms.cosThetaWoH / terms.cosThetaWo * terms.jacobian;
}

float3 evaluateDielectricBsdf(const Material material, const float2 uv, const TexSampleCtx texCtx, const DielectricLobeTerms terms)
{
    if (!terms.isValid)
    {
        return 0.f;
    }
    const float3 baseColor = getMaterialBaseColor(material, uv, texCtx).rgb;
    const float3 tint = terms.isTransmission ? baseColor : material.glossyReflectionTint;
    // Cycles' Multiscatter GGX glass uses the transmission tint as the single-scattering albedo for both lobes
    const float3 multipleScatteringCompensation = material.hasGlossyReflection()
        ? ggxGlassEnergyCompensation(material.roughness, terms.cosThetaWo, material.ior, baseColor)
        : 1.f;
    // D * G2 * |wo.h| * J / cosThetaWo is Cycles' eval, which includes the |cosThetaWi| factor
    return tint * dielectricLobeWeight(terms) * multipleScatteringCompensation * terms.d * terms.g2 * terms.cosThetaWoH *
           terms.jacobian / (terms.cosThetaWo * terms.absCosThetaWi);
}

struct BsdfEval
{
    float3 value;
    float pdf; // in solid angle of wi: the density sampleBsdf draws non-delta samples from
};

// Value and pdf of the full mixture over every lobe that could have produced wi. They are computed together because
// they share their terms and MIS needs them to agree exactly. Delta lobes (roughness = 0) can't be evaluated for
// arbitrary directions and contribute nothing here.
BsdfEval evaluateBsdf(const Material material,
                      const float2 uv,
                      const float3 wo_WS,
                      const float3 wi_WS,
                      const float3 surfNor_WS,
                      const TexSampleCtx texCtx)
{
    BsdfEval result;
    result.value = 0.f;
    result.pdf = 0.f;

    if (material.hasGlossyTransmission())
    {
        const DielectricLobeTerms terms = dielectricLobeTerms(material, wo_WS, wi_WS, surfNor_WS);
        result.value = evaluateDielectricBsdf(material, uv, texCtx, terms);
        result.pdf = dielectricBsdfPdf(terms);
        return result;
    }

    const bool isTransmission = dot(wi_WS, surfNor_WS) < 0.f;
    const float fresnelReflectance = glossyReflectionProbability(material, wo_WS, surfNor_WS);

    if (material.hasDiffuse())
    {
        // Must mirror the hemisphere split in sampleBsdf exactly or MIS breaks silently. Thin-wall diffuse
        // transmission has no Fresnel term on the back side, but the pdf of picking either hemisphere does.
        float3 diffuseNor_WS;
        float diffuseHemisphereWeight;
        float diffuseFresnelWeight;
        if (isTransmission)
        {
            diffuseNor_WS = -surfNor_WS;
            diffuseHemisphereWeight = material.diffuseTransmission;
            diffuseFresnelWeight = 1.f;
        }
        else
        {
            diffuseNor_WS = surfNor_WS;
            diffuseHemisphereWeight = 1.f - material.diffuseTransmission;
            diffuseFresnelWeight = 1.f - fresnelReflectance;
        }

        const float3 diffuseAlbedo = getMaterialBaseColor(material, uv, texCtx).rgb;
        result.value += diffuseAlbedo * M_INV_PI * diffuseHemisphereWeight * diffuseFresnelWeight;
        result.pdf += hemisphereCosineWeightedPdf(wi_WS, diffuseNor_WS) * diffuseHemisphereWeight * (1.f - fresnelReflectance);
    }

    if (material.hasGlossyReflection() && material.roughness > 0.f && !isTransmission)
    {
        const float cosThetaWo = cosTheta(wo_WS, surfNor_WS);
        const float cosThetaWi = cosTheta(wi_WS, surfNor_WS);
        if (cosThetaWo > 0.f && cosThetaWi > 0.f)
        {
            const float alpha = material.roughness * material.roughness;
            const float3 h_WS = normalize(wo_WS + wi_WS);
            const float d = ggxDistribution(alpha, cosTheta(h_WS, surfNor_WS));
            const float g2 = ggxSmithG2(alpha, cosThetaWo, cosThetaWi);
            // The glossy lobe matches Cycles' Glossy BSDF node (Multiscatter GGX): constant Fresnel with the tint as
            // the single-scattering albedo. Any dielectric Fresnel weighting is applied outside the lobe via
            // fresnelReflectance, mirroring the Blender node group's Fresnel-node mix.
            const float3 multipleScatteringCompensation =
                ggxEnergyCompensation(material.roughness, cosThetaWo, material.glossyReflectionTint);
            result.value += material.glossyReflectionTint * fresnelReflectance * multipleScatteringCompensation * d * g2 /
                            (4.f * cosThetaWo * cosThetaWi);
            // VNDF density of the half vector, mapped to wi through the reflection Jacobian
            result.pdf += fresnelReflectance * ggxSmithG1(alpha, cosThetaWo) * d / (4.f * cosThetaWo);
        }
    }

    return result;
}

struct BsdfSample
{
    float3 wi_WS;
    float pdf;
    float3 bsdfValue;
    bool wasSpecular;
    float lobeRoughness; // roughness of the sampled lobe: 1 for diffuse, 0 for delta lobes
};

// A sample with no throughput. It keeps a valid direction so the zero-weight path doesn't feed NaN geometry into
// later bounces.
BsdfSample deadBsdfSample(const float3 wi_WS)
{
    BsdfSample result;
    result.wi_WS = wi_WS;
    result.bsdfValue = 0.f;
    result.pdf = 1.f;
    result.wasSpecular = false;
    result.lobeRoughness = 0.f;
    return result;
}

// Mirrors Cycles' bsdf_microfacet_sample for the glass closure: one VNDF half vector, with the per-microfacet
// Fresnel choosing between reflecting and refracting about it. Total internal reflection at the microfacet
// simply reflects, so no energy is lost to rejected samples.
BsdfSample sampleDielectricBsdf(const Material material,
                                const float2 uv,
                                const float3 wo_WS,
                                const float3 surfNor_WS,
                                const TexSampleCtx texCtx,
                                inout RandomNumberGenerator rng)
{
    if (cosTheta(wo_WS, surfNor_WS) <= 0.f) // no valid microfacets from below the shading normal
    {
        return deadBsdfSample(surfNor_WS);
    }

    const bool isDelta = isDeltaDielectric(material);
    float3 h_WS = surfNor_WS;
    if (!isDelta)
    {
        h_WS = sampleGgxVndf(wo_WS, surfNor_WS, material.roughness * material.roughness, rng); // consumes random numbers, so not a ternary
    }
    const float fresnelReflectance = material.hasGlossyReflection() ? walterFresnel(material.ior, dot(wo_WS, h_WS)) : 0.f;
    const bool chooseReflect = rng.nextFloat() < fresnelReflectance; // nextFloat() is in [0, 1), so F = 0 and F = 1 are exact
    const float3 reflected_WS = normalize(reflect(-wo_WS, h_WS));

    BsdfSample result;
    result.wasSpecular = isDelta;
    result.lobeRoughness = material.roughness;
    if (chooseReflect)
    {
        result.wi_WS = reflected_WS;
    }
    else
    {
        // ior parameter here is ratio of "from medium ior" over "to medium ior"
        // e.g. 1.f / 1.5f for going from air to glass
        const float3 refracted_WS = refract(-wo_WS, h_WS, 1.f / material.ior);
        if (all(refracted_WS == 0.f)) // refract() returns zero on total internal reflection
        {
            // Total internal reflection with no reflection lobe to fall back on (transmission-only materials, e.g.
            // the transmission half of a path split): the sample is lost
            return deadBsdfSample(reflected_WS);
        }
        result.wi_WS = normalize(refracted_WS);
    }

    if (isDelta)
    {
        // The Fresnel weighting is already applied by the random choice of lobe, so the throughput bsdfValue / pdf
        // must reduce to the lobe's tint: pdf is the probability of having chosen this lobe, and bsdfValue carries
        // the same factor so the two cancel
        result.pdf = chooseReflect ? fresnelReflectance : (1.f - fresnelReflectance);
        if (chooseReflect)
        {
            result.bsdfValue = material.glossyReflectionTint * result.pdf;
        }
        else
        {
            result.bsdfValue = getMaterialBaseColor(material, uv, texCtx).rgb * result.pdf;
        }
        return result;
    }

    if ((cosTheta(result.wi_WS, surfNor_WS) > 0.f) != chooseReflect) // sample crossed the surface plane the wrong way
    {
        return deadBsdfSample(result.wi_WS);
    }

    // Use the full lobe value and pdf so the estimator stays consistent with the values NEE uses for MIS
    const DielectricLobeTerms terms = dielectricLobeTerms(material, wo_WS, result.wi_WS, surfNor_WS);
    result.pdf = dielectricBsdfPdf(terms);
    if (result.pdf <= 0.f) // the half vector reconstructed from wi can disagree with the sampled one at float precision
    {
        return deadBsdfSample(result.wi_WS);
    }
    result.bsdfValue = evaluateDielectricBsdf(material, uv, texCtx, terms);
    return result;
}

BsdfSample sampleBsdf(const Material material,
                      const float2 uv,
                      const float3 wo_WS,
                      const float3 surfNor_WS,
                      const TexSampleCtx texCtx,
                      inout RandomNumberGenerator rng)
{
    BsdfSample result;
    result.bsdfValue = 0.f;
    result.wasSpecular = false;
    result.lobeRoughness = 0.f;

    if (!material.canScatter())
    {
        return result;
    }

    if (material.hasGlossyTransmission())
    {
        return sampleDielectricBsdf(material, uv, wo_WS, surfNor_WS, texCtx, rng);
    }

    const float fresnelReflectance = glossyReflectionProbability(material, wo_WS, surfNor_WS);
    const bool chooseReflect = rng.nextFloat() < fresnelReflectance; // nextFloat() is in [0, 1), so F = 0 and F = 1 are exact

    if (chooseReflect)
    {
        result.lobeRoughness = material.roughness;
        if (material.roughness == 0.f)
        {
            result.wi_WS = normalize(reflect(-wo_WS, surfNor_WS));
            // pdf cancels out with the `* fresnelReflectance` in bsdfValue, so actual bsdf value is
            // material.glossyReflectionTint * implicit fresnelReflectance from random chance of choosing reflection
            result.pdf = fresnelReflectance;
            result.bsdfValue = material.glossyReflectionTint * fresnelReflectance;
            result.wasSpecular = true;
            return result;
        }

        const float alpha = material.roughness * material.roughness;
        const float3 h_WS = sampleGgxVndf(wo_WS, surfNor_WS, alpha, rng);
        result.wi_WS = normalize(reflect(-wo_WS, h_WS));
        if (cosTheta(result.wi_WS, surfNor_WS) <= 0.f) // sample fell below the horizon
        {
            return deadBsdfSample(result.wi_WS);
        }
    }
    else
    {
        result.lobeRoughness = 1.f;
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
    const BsdfEval eval = evaluateBsdf(material, uv, wo_WS, result.wi_WS, surfNor_WS, texCtx);
    result.pdf = eval.pdf;
    result.bsdfValue = eval.value;
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

    // Rough glass weights its lobes per microfacet, so a split on the macro-normal Fresnel would mis-weight them;
    // other rough glossy materials could be split but aren't yet
    // TODO: split rough materials too (see #372)
    if (surfMaterial.hasGlossyReflection() &&
        (surfMaterial.hasDiffuseOrGlossyTransmission() || surfMaterial.hasEmission()) && surfMaterial.roughness == 0.f)
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
