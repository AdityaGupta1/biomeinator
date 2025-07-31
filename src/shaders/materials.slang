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

float3 sampleHemisphereCosineWeighted(const float3 normal_WS, const float2 rndSample)
{
    const float r = sqrt(rndSample.x);
    const float theta = M_TWO_PI * rndSample.y;
    const float3 sampledDir_OS = float3(r * cos(theta), r * sin(theta), sqrt(1 - rndSample.x));
    return normalize(mul(computeTBN(normal_WS), sampledDir_OS));
}

float schlickFresnel(const float ior, const float cosThetaWo)
{
    float R0 = (1.f - ior) / (1.f + ior);
    R0 = R0 * R0;
    return R0 + (1.f - R0) * pow(1.f - cosThetaWo, 5.f);
}

float3 evaluateBsdf(
    const Material material,
    const float2 uv,
    const float3 wo_WS,
    const float3 wi_WS,
    const float3 normal_WS)
{
    if (material.hasDiffuse == 1)
    {
        float3 baseColor = material.baseColor;
        if (material.baseColorTextureId != TEXTURE_ID_INVALID)
        {
            baseColor = textures[material.baseColorTextureId].SampleLevel(texSampler, uv, 0).rgb;
        }
        return baseColor * M_INV_PI;
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

    if (material.hasDiffuse == 0 && material.hasSpecularReflection == 0)
    {
        return result;
    }

    bool chooseDiffuse = (material.hasDiffuse == 1);
    if (chooseDiffuse && material.hasSpecularReflection == 1)
    {
        float diffuseWeight = 1.f - schlickFresnel(material.ior, cosTheta(wo_WS, normal_WS));
        chooseDiffuse = (rng.nextFloat() < diffuseWeight);
    }

    if (chooseDiffuse)
    {
        const float3 wi_WS = sampleHemisphereCosineWeighted(normal_WS, rng.nextFloat2());
        result.wi_WS = wi_WS;
        result.pdf = absCosTheta(wi_WS, normal_WS) / M_PI;
        const float3 bsdfValue = evaluateBsdf(material, uv, wo_WS, wi_WS, normal_WS);
        result.bsdfValue = bsdfValue;
    }
    else
    {
        const float3 wi_WS = normalize(reflect(-wo_WS, normal_WS));
        result.wi_WS = wi_WS;
        result.pdf = 1.f;
        result.bsdfValue = material.specularColor;
        result.wasSpecular = true;
    }

    return result;
}
