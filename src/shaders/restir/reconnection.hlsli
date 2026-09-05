// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "util/math.hlsli"

// Reconnection criteria of ReSTIR PT Enhanced (Lin, Kettunen, Wyman 2026, Section 4): a path may
// reconnect at x_j if the sampled lobe at x_{j-1} is rough enough and both the ray footprint at x_j
// and the inverse ray footprint at x_{j-1} exceed a constant multiple of the primary ray footprint.
// Footprints are reciprocal area densities, so the tests bound how much the reconnection segment's
// densities can change when x_{j-1} moves to a neighboring pixel's path.

static const float reconnectionMinLobeRoughness = 0.2f;
static const float reconnectionFootprintScale = 0.0002f; // kappa / 100 with kappa = 0.02 (Eq. 5)

// Constant multiple of the primary ray footprint (Mueller et al. 2021) that both footprint tests
// compare against
float reconnectionFootprintThreshold(const float3 cameraPos_WS, const float3 primaryHitPos_WS, const float3 primaryHitNor_WS)
{
    const float dist2 = distance2(cameraPos_WS, primaryHitPos_WS);
    const float cosTheta = max(absCosTheta(normalize(cameraPos_WS - primaryHitPos_WS), primaryHitNor_WS), 1e-4f);
    return reconnectionFootprintScale * dist2 * (4.f * M_PI) / cosTheta;
}

// The footprint at `to` of a direction sampled at `from` with solid-angle pdf `pdf` is
// 1 / (pdf * G(from -> to)); passes if it is at least `threshold`
bool passesFootprintTest(const float pdf, const float3 from_WS, const float3 to_WS, const float3 toNor_WS, const float threshold)
{
    const float3 toVec = to_WS - from_WS;
    const float dist2 = dot(toVec, toVec);
    if (dist2 <= 0.f)
    {
        return false;
    }
    const float geometryTerm = absCosTheta(toVec * rsqrt(dist2), toNor_WS) / dist2;
    return pdf * geometryTerm * threshold <= 1.f;
}

// Whether x_this can be the reconnection vertex given the sampled lobe at x_prev. `thisPdf` is the
// pdf of the direction sampled at x_this; the inverse test is skipped when x_this scatters
// diffusely only or is a light vertex, since reconnection then does not change that pdf.
bool isReconnectionVertex(const float prevLobeRoughness,
                          const float prevPdf,
                          const float3 prevPos_WS,
                          const float3 prevNor_WS,
                          const float3 thisPos_WS,
                          const float3 thisNor_WS,
                          const float thisPdf,
                          const bool thisIsDelta,
                          const bool thisNeedsInverseTest,
                          const float threshold)
{
    if (prevLobeRoughness < reconnectionMinLobeRoughness || prevPdf <= 0.f)
    {
        return false;
    }
    if (!passesFootprintTest(prevPdf, prevPos_WS, thisPos_WS, thisNor_WS, threshold))
    {
        return false;
    }
    if (thisNeedsInverseTest)
    {
        if (thisIsDelta || thisPdf <= 0.f)
        {
            return false;
        }
        if (!passesFootprintTest(thisPdf, thisPos_WS, prevPos_WS, prevNor_WS, threshold))
        {
            return false;
        }
    }
    return true;
}

// The dome is infinitely far, so only the roughness guard applies (Section 4.2)
bool isDomeReconnectionVertex(const float prevLobeRoughness)
{
    return prevLobeRoughness >= reconnectionMinLobeRoughness;
}
