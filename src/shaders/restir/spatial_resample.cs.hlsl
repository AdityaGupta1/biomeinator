// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_enums.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "restir/pairing.hlsli"
#include "restir/path_reservoir.hlsli"
#include "util/math.hlsli"
#include "util/rng.hlsli"

StructuredBuffer<PathReservoir> reservoirsMergedIn : REGISTER_T(RESTIR, RESERVOIRS_MERGED_IN);
StructuredBuffer<ShiftedPath> shiftedIn : REGISTER_T(RESTIR, SHIFTED_IN);
StructuredBuffer<uint> pairingTextures : REGISTER_T(RESTIR, PAIRING_TEXTURES_IN);
RWStructuredBuffer<PathReservoir> reservoirsOut : REGISTER_U(RESTIR, RESERVOIRS_OUT);
RWStructuredBuffer<float4> pathTracingRawBufferOut : REGISTER_U(RESTIR, PATH_TRACING_RAW_BUFFER_OUT);

// Paired spatial resampling with the confidence-weighted defensive pairwise MIS of GRIS (Lin et al.
// 2022, Eq. 38, with each pHat scaled by its reservoir's M). The pixel's own merged reservoir is the
// canonical sample; each partner's path arrives already shifted into this pixel by the shift pass,
// which also shifted this pixel's path to the partner for the canonical MIS term.
[shader("compute")]
[numthreads(RESTIR_RESAMPLE_WORKGROUP_SIZE_X, RESTIR_RESAMPLE_WORKGROUP_SIZE_Y, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixelIdx = dispatchThreadId.xy;
    if (pixelIdx.x >= renderParams.renderSize.x || pixelIdx.y >= renderParams.renderSize.y)
    {
        return;
    }
    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;
    const uint pixelCount = renderParams.renderSize.x * renderParams.renderSize.y;
    const bool pairWithSelf = (RestirDebugMode)renderParams.restirDebugMode == RestirDebugMode::SPATIAL_SELF;

    const PathReservoir canonical = reservoirsMergedIn[linearPixelIdx];
    const float canonicalPHat = luminance(canonical.F);

    // Partners that exist on screen count toward the neighbor confidence whether or not their shift succeeded
    uint partnerLinearIdx[RESTIR_MAX_SPATIAL_NEIGHBORS];
    bool hasPartner[RESTIR_MAX_SPATIAL_NEIGHBORS];
    float neighborConfidence = 0.f;
    for (uint textureIdx = 0; textureIdx < RESTIR_MAX_SPATIAL_NEIGHBORS; ++textureIdx)
    {
        uint2 partnerIdx;
        hasPartner[textureIdx] = textureIdx < restirParams.spatialNeighborCount &&
            (pairWithSelf || getPairedPixel(pairingTextures, textureIdx, pixelIdx, partnerIdx));
        if (pairWithSelf)
        {
            partnerIdx = pixelIdx;
        }
        partnerLinearIdx[textureIdx] = partnerIdx.y * renderParams.renderSize.x + partnerIdx.x;
        if (hasPartner[textureIdx])
        {
            neighborConfidence += reservoirsMergedIn[partnerLinearIdx[textureIdx]].M;
        }
    }
    const float totalConfidence = canonical.M + neighborConfidence;

    // Canonical MIS weight: its share of every pair, where the partner's view of the canonical path is
    // this pixel's path shifted to the partner
    float canonicalMis = canonical.M / totalConfidence;
    for (uint textureIdx = 0; textureIdx < RESTIR_MAX_SPATIAL_NEIGHBORS; ++textureIdx)
    {
        if (!hasPartner[textureIdx])
        {
            continue;
        }
        const ShiftedPath canonicalAtPartner = shiftedIn[textureIdx * pixelCount + partnerLinearIdx[textureIdx]];
        const float pHatFromPartner = luminance(canonicalAtPartner.F) * canonicalAtPartner.jacobian;
        const float denominator = canonical.M * canonicalPHat + neighborConfidence * pHatFromPartner;
        if (denominator > 0.f)
        {
            canonicalMis += (canonical.M / totalConfidence) * reservoirsMergedIn[partnerLinearIdx[textureIdx]].M * canonicalPHat / denominator;
        }
    }

    PathReservoir selected = canonical;
    float selectedPHat = canonicalPHat;
    float weightSum = canonicalMis * canonicalPHat * canonical.W;
    RandomNumberGenerator rng = initRng(constantParams.rngSeed, 616161, linearPixelIdx, renderParams.frameNumber);

    for (uint textureIdx = 0; textureIdx < RESTIR_MAX_SPATIAL_NEIGHBORS; ++textureIdx)
    {
        if (!hasPartner[textureIdx])
        {
            continue;
        }
        const PathReservoir partner = reservoirsMergedIn[partnerLinearIdx[textureIdx]];
        const ShiftedPath partnerAtCanonical = shiftedIn[textureIdx * pixelCount + linearPixelIdx];
        const float pHat = luminance(partnerAtCanonical.F);
        if (pHat <= 0.f || partnerAtCanonical.jacobian <= 0.f || partner.W <= 0.f)
        {
            continue;
        }
        // The partner's own target evaluated at its path, mapped into this pixel's measure
        const float pHatFromPartner = luminance(partner.F) / partnerAtCanonical.jacobian;
        const float denominator = canonical.M * pHat + neighborConfidence * pHatFromPartner;
        const float mis = (neighborConfidence / totalConfidence) * partner.M * pHatFromPartner / denominator;
        const float weight = mis * pHat * partner.W * partnerAtCanonical.jacobian;
        if (weight <= 0.f)
        {
            continue;
        }

        weightSum += weight;
        if (rng.nextFloat() * weightSum < weight)
        {
            selected = partner;
            selected.F = partnerAtCanonical.F;
            selected.rcJacobianTerms = partnerAtCanonical.rcJacobianTerms;
            selectedPHat = pHat;
        }
    }

    selected.W = (weightSum > 0.f && selectedPHat > 0.f) ? weightSum / selectedPHat : 0.f;
    selected.M = totalConfidence;

    const uint slotIdx = linearPixelIdx * (bool(renderParams.doPathSplitting) ? 2 : 1);
    reservoirsOut[slotIdx] = selected;
    pathTracingRawBufferOut[slotIdx].xyz += selected.F * selected.W;
}
