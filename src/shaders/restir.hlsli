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

#include "light_sampling.hlsli"

// TODO: make this into a setting
//#define RIS_NUM_CANDIDATES 32
#define RIS_NUM_CANDIDATES 16

float risTargetFunction(const float3 surfPos_WS, const float3 surfNor_WS, const float3 pointOnLight_WS)
{
    // TODO: include light emission somehow?
    const float cosThetaSurf = absCosTheta(normalize(pointOnLight_WS - surfPos_WS), surfNor_WS);
    return cosThetaSurf;
}

DirectLightingSample sampleDirectLightingRis(const float3 surfPos_WS, const float3 surfNor_WS, inout RandomSampler rng)
{
    DirectLightingSample result;
    result.didHitLight = false;

    uint Y_lightIdx = ~0u;
    float Y_p_hat = 0.f;
    float3 Y_pointOnLight_WS = 0.f;
    float w_sum = 0.f;

    for (int risCandidateIdx = 0; risCandidateIdx < RIS_NUM_CANDIDATES; ++risCandidateIdx)
    {
        float lightPickPdf;
        uint lightIdx;
        const AreaLight light = pickLightUniform(rng, lightPickPdf, lightIdx);

        float lightSamplePdf;
        const float3 pointOnLight_WS = samplePointOnLight(light, rng, lightSamplePdf);

        const float m_i = 1.f / RIS_NUM_CANDIDATES;
        const float p_hat = risTargetFunction(surfPos_WS, surfNor_WS, pointOnLight_WS);
        const float r2 = distance2(surfPos_WS, pointOnLight_WS);
        lightSamplePdf *= r2 / absCosTheta(normalize(surfPos_WS - pointOnLight_WS), light.normal_WS);
        const float p_i = lightPickPdf * lightSamplePdf;
        const float W_X_i = 1.f / p_i;

        const float w_i = m_i * p_hat * W_X_i;

        w_sum += w_i;
        if (rng.nextFloat() < w_i / w_sum)
        {
            Y_lightIdx = lightIdx;
            Y_p_hat = p_hat;
            Y_pointOnLight_WS = pointOnLight_WS;
        }
    }

    if (Y_lightIdx == ~0u)
    {
        return result;
    }

    const AreaLight light = areaLights[Y_lightIdx];
    const float W_Y = w_sum / Y_p_hat; // unbiased contribution weight

    result.wi_WS = normalize(Y_pointOnLight_WS - surfPos_WS);

    // TODO: deduplicate code shared with sampleDirectLightingUniform
    RayDesc ray;
    ray.Origin = surfPos_WS + RAY_ORIGIN_OFFSET_EPSILON * surfNor_WS;
    ray.Direction = result.wi_WS;
    ray.TMin = 0.f;
    ray.TMax = 10000.f;

    Payload lightPayload;
    lightPayload.materialIdx = MATERIAL_IDX_INVALID;
    TraceRay(raytracingAcs, RAY_FLAG_NONE, 0xFF, PT_HITGROUP_LIGHTS, 0, 0, ray, lightPayload);

    if (lightPayload.materialIdx == MATERIAL_IDX_INVALID || lightPayload.hitInfo.instanceId != light.instanceId || lightPayload.hitInfo.triangleIdx != light.triangleIdx)
    {
        return result;
    }

    result.didHitLight = true;
    const Material material = materials[lightPayload.materialIdx];
    result.Le = material.getEmissiveColor();
    result.W_Y = W_Y;
    result.p_hat = Y_p_hat;

    return result;
}
