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

#include "util/math.slang"
#include "util/rng.slang"

StructuredBuffer<Material> materials : REGISTER_T(REGISTER_MATERIALS, REGISTER_SPACE_BUFFERS);

Texture2D<float4> textures[MAX_NUM_TEXTURES] : REGISTER_T(REGISTER_TEXTURES, REGISTER_SPACE_TEXTURES);
SamplerState texSampler : REGISTER_S(REGISTER_TEX_SAMPLER, REGISTER_SPACE_TEXTURES);

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

float3 evaluateBsdf<let calculateFresnelReflectance : bool>(
    const Material material,
    const float2 uv,
    const float3 wo_WS,
    const float3 wi_WS,
    const float3 normal_WS,
    float fresnelReflectance = 0.f)
{
    if (material.hasDiffuse())
    {
        float3 baseColor = material.baseColor;
        if (material.baseColorTextureId != TEXTURE_ID_INVALID)
        {
            baseColor = textures[material.baseColorTextureId].SampleLevel(texSampler, uv, 0).rgb;
        }

        if (calculateFresnelReflectance && material.hasSpecularReflection())
        {
            fresnelReflectance = walterFresnel(material.ior, cosTheta(wo_WS, normal_WS));
        }

        return baseColor * M_INV_PI * (1.f - fresnelReflectance);
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
    const float3 normal_WS,
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
        fresnelReflectance = walterFresnel(material.ior, cosTheta(wo_WS, normal_WS));
        chooseReflect = (rng.nextFloat() < fresnelReflectance);
    }

    if (chooseReflect)
    {
        const float3 wi_WS = normalize(reflect(-wo_WS, normal_WS));
        result.wi_WS = wi_WS;
        result.pdf = fresnelReflectance;
        result.bsdfValue = material.specularColor * fresnelReflectance;
        result.wasSpecular = true;
    }
    else
    {
        const float3 wi_WS = sampleHemisphereCosineWeighted(normal_WS, rng);
        result.wi_WS = wi_WS;
        result.pdf = absCosTheta(wi_WS, normal_WS) * (1.f - fresnelReflectance) * M_INV_PI;
        const float3 bsdfValue = evaluateBsdf<false /*calculateFresnelReflectance*/>(material, uv, wo_WS, wi_WS, normal_WS, fresnelReflectance);
        result.bsdfValue = bsdfValue;
    }

    return result;
}

float bsdfPdf(
    const Material material,
    const float3 wo_WS,
    const float3 wi_WS,
    const float3 normal_WS)
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

    float pdf = absCosTheta(wi_WS, normal_WS) * M_INV_PI;

    if (canReflect)
    {
        const float fresnelReflectance = walterFresnel(material.ior, cosTheta(wo_WS, normal_WS));
        pdf *= (1.f - fresnelReflectance);
    }

    return pdf;
}
