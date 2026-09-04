// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "util/math.hlsli"
#include "util/rng.hlsli"

// Weighted reservoir sampling over the complete paths of one path tree (initial resampling
// in ReSTIR PT). Each candidate's F is its full contribution as the path tracer already
// estimates it (throughput, radiance, path MIS weight, divided by sampling pdfs), so the
// primary sample space source pdf is 1 and the resampling weight is just the target
// pHat = luminance(F). Candidates cover disjoint parts of path space (different lengths and
// sampling techniques), so their resampling MIS weights are all 1.
struct PathTreeReservoir
{
    float3 selectedF;
    float weightSum;
    RandomNumberGenerator rng;

    void addCandidate(const float3 F)
    {
        const float weight = luminance(F);
        if (weight <= 0.f)
        {
            return;
        }

        weightSum += weight;
        if (rng.nextFloat() * weightSum < weight)
        {
            selectedF = F;
        }
    }

    // F(selected) * W, with W = weightSum / pHat(selected)
    float3 resolve()
    {
        if (weightSum <= 0.f)
        {
            return 0.f;
        }
        return selectedF * (weightSum / luminance(selectedF));
    }
};

PathTreeReservoir initPathTreeReservoir(const RandomNumberGenerator rng)
{
    PathTreeReservoir reservoir;
    reservoir.selectedF = 0.f;
    reservoir.weightSum = 0.f;
    reservoir.rng = rng;
    return reservoir;
}
