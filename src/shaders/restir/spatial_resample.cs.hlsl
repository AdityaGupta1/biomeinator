// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_enums.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "restir/pairing.hlsli"
#include "restir/pairwise_mis.hlsli"
#include "restir/path_reservoir.hlsli"
#include "util/math.hlsli"
#include "util/rng.hlsli"

StructuredBuffer<PathReservoir> reservoirsMergedIn : REGISTER_T(RESTIR, RESERVOIRS_MERGED_IN);
StructuredBuffer<ShiftedPath> shiftedIn : REGISTER_T(RESTIR, SHIFTED_IN);
StructuredBuffer<uint> pairingTextures : REGISTER_T(RESTIR, PAIRING_TEXTURES_IN);
RWStructuredBuffer<PathReservoir> reservoirsHistoryOut : REGISTER_U(RESTIR, RESERVOIRS_HISTORY_OUT);
RWStructuredBuffer<float4> pathTracingRawBufferOut : REGISTER_U(RESTIR, PATH_TRACING_RAW_BUFFER_OUT);
RWStructuredBuffer<uint> reservoirSeedsOut : REGISTER_U(RESTIR, RESERVOIR_SEEDS_OUT);
StructuredBuffer<float> duplicationMapIn : REGISTER_T(RESTIR, DUPLICATION_MAP_IN); // previous frame's, for the debug view

// Paired spatial resampling with pairwise MIS (restir/pairwise_mis.hlsli). The pixel's own reservoir
// after temporal reuse is the canonical sample; each partner's path arrives already shifted into
// this pixel by the shift pass, which also shifted this pixel's path to the partner for the
// canonical MIS term. The result becomes next frame's temporal history. Shading uses the
// vector-valued resampling weights (Lin et al. 2026, Section 6.3): the RGB sum of every candidate's
// weighted contribution has the same expectation as F * W of the selected path but averages the
// partners' uncorrelated chroma noise, which scalar selection by luminance leaves in place.
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
            neighborConfidence += reservoirM(reservoirsMergedIn[partnerLinearIdx[textureIdx]]);
        }
    }
    const float canonicalM = reservoirM(canonical);
    const float totalConfidence = canonicalM + neighborConfidence;

    // Canonical MIS weight: its share of every pair, where the partner's view of the canonical path is
    // this pixel's path shifted to the partner
    float canonicalMis = canonicalM / totalConfidence;
    for (uint textureIdx = 0; textureIdx < RESTIR_MAX_SPATIAL_NEIGHBORS; ++textureIdx)
    {
        if (!hasPartner[textureIdx])
        {
            continue;
        }
        const ShiftedPath canonicalAtPartner = shiftedIn[textureIdx * pixelCount + partnerLinearIdx[textureIdx]];
        const float pHatFromPartner = luminance(canonicalAtPartner.F) * canonicalAtPartner.jacobian;
        canonicalMis += pairwiseMisCanonicalTerm(canonicalM, reservoirM(reservoirsMergedIn[partnerLinearIdx[textureIdx]]), neighborConfidence,
            totalConfidence, canonicalPHat, pHatFromPartner);
    }

    PathReservoir selected = canonical;
    float selectedPHat = canonicalPHat;
    float weightSum = canonicalMis * canonicalPHat * canonical.W;
    float3 shadedSum = canonicalMis * canonical.F * canonical.W;
    RandomNumberGenerator rng = initRng(constantParams.rngSeed, 616161, linearPixelIdx, renderParams.frameNumber);
    uint partnersOnScreen = 0;
    uint spatialShiftsSucceeded = 0;

    for (uint textureIdx = 0; textureIdx < RESTIR_MAX_SPATIAL_NEIGHBORS; ++textureIdx)
    {
        if (!hasPartner[textureIdx])
        {
            continue;
        }
        const PathReservoir partner = reservoirsMergedIn[partnerLinearIdx[textureIdx]];
        const ShiftedPath partnerAtCanonical = shiftedIn[textureIdx * pixelCount + linearPixelIdx];
        const float pHat = luminance(partnerAtCanonical.F);
        ++partnersOnScreen;
        if (pHat <= 0.f || partnerAtCanonical.jacobian <= 0.f || partner.W <= 0.f)
        {
            continue;
        }
        ++spatialShiftsSucceeded;
        // The partner's own target evaluated at its path, mapped into this pixel's measure
        const float pHatFromPartner = luminance(partner.F) / partnerAtCanonical.jacobian;
        const float mis = pairwiseMisNeighbor(canonicalM, reservoirM(partner), neighborConfidence, totalConfidence, pHat, pHatFromPartner);
        const float weight = mis * pHat * partner.W * partnerAtCanonical.jacobian;
        if (weight <= 0.f)
        {
            continue;
        }

        weightSum += weight;
        shadedSum += mis * partnerAtCanonical.F * partner.W * partnerAtCanonical.jacobian;
        if (rng.nextFloat() * weightSum < weight)
        {
            selected = partner;
            selected.F = partnerAtCanonical.F;
            selected.rcJacobianTerms = partnerAtCanonical.rcJacobianTerms;
            selectedPHat = pHat;
        }
    }

    selected.W = (weightSum > 0.f && selectedPHat > 0.f) ? weightSum / selectedPHat : 0.f;
    setReservoirM(selected, totalConfidence);

    reservoirsHistoryOut[linearPixelIdx] = selected;
    reservoirSeedsOut[linearPixelIdx] = (selected.W > 0.f) ? selected.seed : 0u;

    const uint slotIdx = linearPixelIdx * (bool(renderParams.doPathSplitting) ? 2 : 1);
    const RestirDebugMode debugMode = (RestirDebugMode)renderParams.restirDebugMode;
    if (debugMode == RestirDebugMode::CONFIDENCE)
    {
        pathTracingRawBufferOut[slotIdx].xyz = reservoirM(selected) / 100.f;
        return;
    }
    if (debugMode == RestirDebugMode::DUPLICATION)
    {
        pathTracingRawBufferOut[slotIdx].xyz = duplicationMapIn[linearPixelIdx];
        return;
    }
    if (debugMode == RestirDebugMode::SHIFT_SUCCESS)
    {
        pathTracingRawBufferOut[slotIdx].xyz = float3(
            bool(canonical.debugFlags & RESERVOIR_DEBUG_TEMPORAL_SHIFT_SUCCEEDED) ? 1.f : 0.f,
            partnersOnScreen > 0 ? float(spatialShiftsSucceeded) / float(partnersOnScreen) : 0.f,
            float(partnersOnScreen) / float(max(restirParams.spatialNeighborCount, 1u)));
        return;
    }
    pathTracingRawBufferOut[slotIdx].xyz += shadedSum;
}
