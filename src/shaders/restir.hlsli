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

#define RIS_MAX_NUM_LIGHT_CANDIDATES 32u
#define RIS_MAX_NUM_BSDF_CANDIDATES 1u
#define RIS_MIN_NUM_LIGHT_CANDIDATES 8u
#define RIS_MIN_NUM_BSDF_CANDIDATES 1u

float risTargetFunction(const AreaLight light, const float3 surfPos_WS, const float3 surfNor_WS, const float3 pointOnLight_WS, const Material material, const float2 uv, const float3 wo_WS)
{
    const float3 wi_WS = normalize(pointOnLight_WS - surfPos_WS);

    const Material lightMaterial = materials[light.materialIdx];

    // const float3 bsdfVal = evaluateBsdf(material, uv, wo_WS, wi_WS, surfNor_WS, true /*calculateFresnelReflectance*/); // TODO: should this be included here, or just a proxy to save performance?

    const float cosThetaSurf = absCosTheta(wi_WS, surfNor_WS); // TODO: specify if light is single or double sided and use cosTheta or absCosTheta accordingly

    // return luminance(lightMaterial.getEmissiveColor() * bsdfVal) * cosThetaSurf;
    return luminance(lightMaterial.getEmissiveColor()) * cosThetaSurf;
}

RisSample generateDirectLightingRisSample(const uint hitGroup, const float3 surfPos_WS, const float3 surfNor_WS, const Material material, const float2 uv, const float3 wo_WS, const bool isFirstNonDeltaSurface, inout RandomSampler rng)
{
    const uint numLightCandidates = isFirstNonDeltaSurface ? RIS_MAX_NUM_LIGHT_CANDIDATES : RIS_MIN_NUM_LIGHT_CANDIDATES;
    const uint numBsdfCandidates = isFirstNonDeltaSurface ? RIS_MAX_NUM_BSDF_CANDIDATES : RIS_MIN_NUM_BSDF_CANDIDATES;

    uint Y_lightIdx = ~0u;
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
        const float misDenominator = numLightCandidates * lightPdf + numBsdfCandidates * bsdfPdf(material, wo_WS, wi_WS, surfNor_WS);
        // const float m_i = lightPdf / misDenominator;
        // const float W_X_i = 1.f / lightPdf;

        const float p_hat = risTargetFunction(light, surfPos_WS, surfNor_WS, pointOnLight_WS, material, uv, wo_WS);

        const float w_i = p_hat / misDenominator;
        // const float w_i = m_i * p_hat * W_X_i;

        w_sum += w_i;
        if (rng.nextFloat() < w_i / w_sum)
        {
            Y_lightIdx = lightIdx;
            Y_p_hat = p_hat;
            Y_pointOnLight_WS = pointOnLight_WS;
        }
    }

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
        ray.TMax = 10000.f;

        Payload lightPayload;
        lightPayload.flags = 0;
        TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, hitGroup, 0, 0, ray, lightPayload);

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

        const float p_hat = risTargetFunction(light, surfPos_WS, surfNor_WS, pointOnLight_WS, material, uv, wo_WS);

        const float w_i = p_hat / misDenominator;
        // const float w_i = m_i * p_hat * W_X_i;

        w_sum += w_i;
        if (rng.nextFloat() < w_i / w_sum)
        {
            Y_lightIdx = areaLightIdx;
            Y_p_hat = p_hat;
            Y_pointOnLight_WS = pointOnLight_WS;
        }
    }

    RisSample risSample;
    risSample.lightIdx = Y_lightIdx;
    risSample.pointOnLight_WS = Y_pointOnLight_WS;
    risSample.W = w_sum / Y_p_hat;
    return risSample;
}

DirectLightingSample evaluateRisSample(const RisSample risSample, const float3 surfPos_WS, const float3 surfNor_WS)
{
    DirectLightingSample result;
    result.didHitLight = false;

    if (risSample.lightIdx == LIGHT_IDX_INVALID)
    {
        return result;
    }

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
