// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "common/global_params.hlsli"
#include "light/light_sampling.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

#define RIS_MAX_NUM_LIGHT_CANDIDATES 32
#define RIS_MIN_NUM_LIGHT_CANDIDATES 8

// Area-measure target function: includes the full geometric term (cos terms and 1/r^2) so
// reservoirs can be reused across surfaces without a solid-angle Jacobian, and so distant
// lights are correctly penalized during resampling.
float risTargetFunction(const AreaLight light, const float3 pointOnLight_WS, const float3 surfPos_WS, const float3 surfNor_WS)
{
    const float3 wi_WS = normalize(pointOnLight_WS - surfPos_WS);

    const Material lightMaterial = materials[light.materialIdx];

    // const float3 bsdfVal = evaluateBsdf(material, uv, wo_WS, wi_WS, surfNor_WS); // TODO: should this be included here, or maybe a proxy to save performance?

    const float cosThetaSurf = absCosTheta(wi_WS, surfNor_WS); // TODO: replace with cosTheta for non-transmissive materials? (will require more complex MIS weights for RIS sample generation)

    float3 lightNor_WS;
    float lightArea_unused;
    getLightNormalAndArea(light, lightNor_WS, lightArea_unused);
    const float cosThetaLight = absCosTheta(-wi_WS, lightNor_WS);

    const float r2 = distance2(surfPos_WS, pointOnLight_WS);

    // return luminance(lightMaterial.getEmissiveColor() * bsdfVal) * cosThetaSurf * cosThetaLight / r2;
    return (r2 > 0.f) ? (lightMaterial.emissiveStrength * cosThetaSurf * cosThetaLight / r2) : 0.f;
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
        float lightPdf_unused;
        uint lightIdx;
        const AreaLight light = sampleLightUniform(surfPos_WS, rng, pointOnLight_WS, lightPdf_unused, lightIdx);

        // area-measure source pdf to match the area-measure target function
        float3 lightNor_unused;
        float lightArea;
        getLightNormalAndArea(light, lightNor_unused, lightArea);
        const float lightPdf = 1.f / (sceneParams.numAreaLights * lightArea);

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

    // risSample.W is an area-measure inverse pdf; convert to solid angle for shading,
    // which expects pdfOrW_Y to be 1 / pdf_solidAngle
    float3 lightNor_WS;
    float lightArea_unused;
    getLightNormalAndArea(areaLights[risSample.lightIdx], lightNor_WS, lightArea_unused);
    const float cosThetaLight = absCosTheta(-result.wi_WS, lightNor_WS);
    const float r2 = distance2(surfPos_WS, risSample.pointOnLight_WS);
    result.pdfOrW_Y = (r2 > 0.f) ? (risSample.W * cosThetaLight / r2) : 0.f;

    return result;
}
#endif // HITGROUP_LIGHTS
