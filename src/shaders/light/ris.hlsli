// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "common/global_params.hlsli"
#include "light/light_sampling.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

#define RIS_MAX_NUM_LIGHT_CANDIDATES 32
#define RIS_MIN_NUM_LIGHT_CANDIDATES 8

float risTargetFunction(const AreaLight light, const float3 pointOnLight_WS, const float3 surfPos_WS, const float3 surfNor_WS)
{
    const float3 wi_WS = normalize(pointOnLight_WS - surfPos_WS);

    const Material lightMaterial = materials[light.materialIdx];

    // const float3 bsdfVal = evaluateBsdf(material, uv, wo_WS, wi_WS, surfNor_WS); // TODO: should this be included here, or maybe a proxy to save performance?

    const float cosThetaSurf = absCosTheta(wi_WS, surfNor_WS); // TODO: replace with cosTheta for non-transmissive materials? (will require more complex MIS weights for RIS sample generation)

    // return luminance(lightMaterial.getEmissiveColor() * bsdfVal) * cosThetaSurf;
    return lightMaterial.emissiveStrength * cosThetaSurf;
}

float calcGeomTermJacobian(const float3 this_surfPos_WS, const float3 other_surfPos_WS, const float3 pointOnLight_WS, const float3 lightNor_WS)
{
    const float3 this_wi_WS = normalize(pointOnLight_WS - this_surfPos_WS);
    const float this_r2 = distance2(this_surfPos_WS, pointOnLight_WS);

    const float3 other_wi_WS = normalize(pointOnLight_WS - other_surfPos_WS);
    const float other_r2 = distance2(other_surfPos_WS, pointOnLight_WS);

    const float geomTermJacobian = (absCosTheta(-this_wi_WS, lightNor_WS) * other_r2) / (absCosTheta(-other_wi_WS, lightNor_WS) * this_r2);
    return (isinf(geomTermJacobian) || isnan(geomTermJacobian)) ? 0.f : geomTermJacobian;
}

RisSample generateDirectLightingRisSample(const float3 surfPos_WS,
                                          const float3 surfNor_WS,
                                          const Material material,
                                          const float2 uv,
                                          const float3 wo_WS,
                                          const bool isFirstNonDeltaSurface,
                                          inout RandomNumberGenerator rng,
                                          out bool isBsdfSample)
{
    const uint numLightCandidates = isFirstNonDeltaSurface ? RIS_MAX_NUM_LIGHT_CANDIDATES : RIS_MIN_NUM_LIGHT_CANDIDATES;

    isBsdfSample = false;

    uint Y_lightIdx = LIGHT_IDX_INVALID;
    float Y_p_hat = 0.f;
    float3 Y_pointOnLight_WS = 0.f;
    float w_sum = 0.f;
    for (uint risLightCandidateIdx = 0; risLightCandidateIdx < numLightCandidates; ++risLightCandidateIdx)
    {
        float3 pointOnLight_WS;
        float lightPdf;
        uint lightIdx;
        const AreaLight light = sampleLightUniform(surfPos_WS, rng, pointOnLight_WS, lightPdf, lightIdx);

        const float3 wi_WS = normalize(pointOnLight_WS - surfPos_WS);
        const float misDenominator = numLightCandidates * lightPdf;
        // const float m_i = lightPdf / misDenominator;
        // const float W_X_i = 1.f / lightPdf;
        // misDenominator = 1.f / (m_i * W_X_i)

        const float p_hat = risTargetFunction(light, pointOnLight_WS, surfPos_WS, surfNor_WS);

        const float w_i = p_hat / misDenominator;
        //              = m_i * p_hat * W_X_i

        w_sum += w_i;
        if (rng.nextFloat() < w_i / w_sum)
        {
            Y_lightIdx = lightIdx;
            Y_p_hat = p_hat;
            Y_pointOnLight_WS = pointOnLight_WS;
        }
    }

    RisSample risSampleOut;
    risSampleOut.lightIdx = Y_lightIdx;
    risSampleOut.pointOnLight_WS = Y_pointOnLight_WS;
    risSampleOut.W = sanitizeFloat(w_sum / Y_p_hat, 0.f);
    risSampleOut.p_hat = Y_p_hat;
    risSampleOut.confidence = 1;
    risSampleOut.pad0 = 0;
    return risSampleOut;
}

#ifdef HITGROUP_LIGHTS
DirectLightingSample evaluateRisSample(const RisSample risSample,
                                       const float3 surfPos_WS,
                                       const float3 surfNor_WS,
                                       const RayCone rayCone,
                                       const bool canPassthrough,
                                       const bool startUnderwater,
                                       inout RandomNumberGenerator rng)
{
    DirectLightingSample result;
    result.lightIdx = risSample.lightIdx;
    result.didHitLight = false;

    if (result.lightIdx == LIGHT_IDX_INVALID)
    {
        return result;
    }

    result.pointOnLight_WS = risSample.pointOnLight_WS;
    result.wi_WS = normalize(risSample.pointOnLight_WS - surfPos_WS);

    float3 Le;
    const bool didHitLight = traceToLight(surfPos_WS,
                                          surfNor_WS,
                                          result.wi_WS,
                                          risSample.pointOnLight_WS,
                                          areaLights[risSample.lightIdx],
                                          rayCone,
                                          canPassthrough,
                                          startUnderwater,
                                          rng,
                                          Le);
    if (!didHitLight)
    {
        return result;
    }

    result.didHitLight = true;
    result.Le = Le;
    result.pdfOrW_Y = risSample.W;
    return result;
}
#endif // HITGROUP_LIGHTS
