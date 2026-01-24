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

#define RIS_MAX_NUM_BSDF_CANDIDATES 0
#define RIS_MIN_NUM_BSDF_CANDIDATES 0
#define DO_BSDF_SAMPLES (RIS_MIN_NUM_BSDF_CANDIDATES > 0 || RIS_MAX_NUM_BSDF_CANDIDATES > 0)

float risTargetFunction(const AreaLight light, const float3 pointOnLight_WS, const float3 surfPos_WS, const float3 surfNor_WS)
{
    const float3 wi_WS = normalize(pointOnLight_WS - surfPos_WS);

    const Material lightMaterial = materials[light.materialIdx];

    // const float3 bsdfVal = evaluateBsdf(material, uv, wo_WS, wi_WS, surfNor_WS); // TODO: should this be included here, or maybe a proxy to save performance?

    const float cosThetaSurf = absCosTheta(wi_WS, surfNor_WS); // TODO: replace with cosTheta for non-transmissive materials? (will require more complex MIS weights for RIS sample generation)

    // return luminance(lightMaterial.getEmissiveColor() * bsdfVal) * cosThetaSurf;
    return lightMaterial.emissiveStrength * cosThetaSurf;
}

#ifdef HITGROUP_LIGHTS
RisSample generateDirectLightingRisSample(const float3 surfPos_WS,
                                          const float3 surfNor_WS,
                                          const Material material,
                                          const float2 uv,
                                          const float3 wo_WS,
                                          const bool isFirstNonDeltaSurface,
                                          inout RandomSampler rng,
                                          out bool isBsdfSample)
{
    const uint numLightCandidates = isFirstNonDeltaSurface ? RIS_MAX_NUM_LIGHT_CANDIDATES : RIS_MIN_NUM_LIGHT_CANDIDATES;
    const uint numBsdfCandidates = isFirstNonDeltaSurface ? RIS_MAX_NUM_BSDF_CANDIDATES : RIS_MIN_NUM_BSDF_CANDIDATES;

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
#if DO_BSDF_SAMPLES
        const float misDenominator = numLightCandidates * lightPdf + numBsdfCandidates * bsdfPdf(material, wo_WS, wi_WS, surfNor_WS);
#else
        const float misDenominator = numLightCandidates * lightPdf;
#endif
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

#if DO_BSDF_SAMPLES
    for (uint risBsdfCandidateIdx = 0; risBsdfCandidateIdx < numBsdfCandidates; ++risBsdfCandidateIdx)
    {
        const BsdfSample bsdfSample = sampleBsdf(material, uv, wo_WS, surfNor_WS, rng);

        if (bsdfSample.wasSpecular)
        {
            continue;
        }

        RayDesc ray;
        ray.Origin = surfPos_WS + RAY_ORIGIN_OFFSET_EPSILON * surfNor_WS;
        ray.Direction = bsdfSample.wi_WS;
        ray.TMin = 0.f;
        ray.TMax = RAY_DEFAULT_TMAX;

        Payload lightPayload;
        lightPayload.flags = 0;
        TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, HITGROUP_LIGHTS, 0, 0, ray, lightPayload);

        if (!bool(lightPayload.flags & PAYLOAD_FLAG_DID_HIT))
        {
            continue;
        }

        const InstanceData instanceData = instanceDatas[lightPayload.hitInfo.instanceId];
        const PerTriangleData perTriData = perTriDatas[instanceData.perTriDatasBufferOffset + lightPayload.hitInfo.triangleIdx];
        if (perTriData.localAreaLightIdx == LIGHT_IDX_INVALID)
        {
            continue;
        }

        const uint areaLightIdx = instanceData.areaLightsBufferOffset + perTriData.localAreaLightIdx;
        const AreaLight light = areaLights[areaLightIdx];
        const float3 pointOnLight_WS = lightPayload.hitInfo.hitPos_WS;

        const float misDenominator = numLightCandidates * lightPdfUniform(lightPayload.hitInfo, surfPos_WS, bsdfSample.wi_WS) + numBsdfCandidates * bsdfSample.pdf;
        // const float m_i = bsdfSample.pdf / misDenominator
        // const float W_X_i = 1.f / bsdfSample.pdf;

        const float p_hat = risTargetFunction(light, pointOnLight_WS, surfPos_WS, surfNor_WS);

        const float w_i = p_hat / misDenominator;
        // const float w_i = m_i * p_hat * W_X_i;

        w_sum += w_i;
        if (rng.nextFloat() < w_i / w_sum)
        {
            Y_lightIdx = areaLightIdx;
            Y_p_hat = p_hat;
            Y_pointOnLight_WS = pointOnLight_WS;
            isBsdfSample = true;
        }
    }
#endif

    RisSample risSampleOut;
    risSampleOut.lightIdx = Y_lightIdx;
    risSampleOut.pointOnLight_WS = Y_pointOnLight_WS;
    risSampleOut.W = sanitizeFloat(w_sum / Y_p_hat, 0.f);
    risSampleOut.p_hat = Y_p_hat;
    risSampleOut.confidence = 1;
    return risSampleOut;
}

DirectLightingSample evaluateRisSample(const RisSample risSample, const float3 surfPos_WS, const float3 surfNor_WS)
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
    if (!traceToLight(surfPos_WS, surfNor_WS, result.wi_WS, risSample.pointOnLight_WS, areaLights[risSample.lightIdx], Le))
    {
        return result;
    }

    result.didHitLight = true;
    result.Le = Le;
    result.pdfOrW_Y = risSample.W;
    return result;
}
#endif
