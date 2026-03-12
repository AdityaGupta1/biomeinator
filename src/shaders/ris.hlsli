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

#include "global_params.hlsli"
#include "light_sampling.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

#define RIS_MAX_NUM_LIGHT_CANDIDATES 32
#define RIS_MIN_NUM_LIGHT_CANDIDATES 8

struct RisSample
{
    uint lightIdx;
    float3 pointOnLight_WS;
    float W;
};

float risTargetFunction(const AreaLight light, const float3 pointOnLight_WS, const float3 surfPos_WS, const float3 surfNor_WS)
{
    const float3 wi_WS = normalize(pointOnLight_WS - surfPos_WS);

    const Material lightMaterial = materials[light.materialIdx];

    // const float3 bsdfVal = evaluateBsdf(material, uv, wo_WS, wi_WS, surfNor_WS); // TODO: should this be included here, or maybe a proxy to save performance?

    const float cosThetaSurf = absCosTheta(wi_WS, surfNor_WS); // TODO: replace with cosTheta for non-transmissive materials? (will require more complex MIS weights for RIS sample generation)

    // return luminance(lightMaterial.getEmissiveColor() * bsdfVal) * cosThetaSurf;
    return lightMaterial.emissiveStrength * cosThetaSurf;
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
    return risSampleOut;
}

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
