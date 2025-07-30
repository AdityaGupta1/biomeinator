#pragma once

#include "../rendering/common/common_structs.h"
#include "../rendering/common/common_registers.h"

#include "util/math.slang"

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
    float3 baseColor = material.baseColor;
    if (material.baseColorTextureId != TEXTURE_ID_INVALID)
    {
        baseColor = textures[material.baseColorTextureId].SampleLevel(texSampler, uv, 0).rgb;
    }

    return baseColor * M_INV_PI;
}

struct BsdfSample
{
    float3 wi_WS;
    float pdf;

    float3 bsdfValue;
};

BsdfSample sampleBsdf(
    const Material material,
    const float2 uv,
    const float3 wo_WS,
    const float3 normal_WS,
    float2 rndSample)
{
    BsdfSample result;

    const float3 wi_WS = sampleHemisphereCosineWeighted(normal_WS, rndSample);
    result.wi_WS = wi_WS;

    float3 bsdfValue = evaluateBsdf(material, uv, wo_WS, wi_WS, normal_WS);

    result.pdf = absCosTheta(wi_WS, normal_WS) / M_PI;
    result.bsdfValue = bsdfValue;

    return result;
}
