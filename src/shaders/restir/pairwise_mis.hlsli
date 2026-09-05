// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// Confidence-weighted defensive pairwise MIS (GRIS, Lin et al. 2022, Eq. 38, with each pHat scaled
// by its reservoir's M). One canonical sample with confidence Mc is paired with every neighbor i;
// N is the neighbors' total confidence and Mtot = Mc + N. The weights sum to 1 over the canonical
// and all neighbors evaluated at the same sample.

// Weight of neighbor i's sample y. `pHat` is the canonical target at y; `pHatFromNeighbor` is the
// neighbor's own target at its sample mapped into the canonical measure, pHat_i(x_i) / |dT_i/dx_i|.
float pairwiseMisNeighbor(const float Mc, const float Mi, const float N, const float Mtot, const float pHat, const float pHatFromNeighbor)
{
    const float denominator = Mc * pHat + N * pHatFromNeighbor;
    if (denominator <= 0.f)
    {
        return 0.f;
    }
    return (N / Mtot) * Mi * pHatFromNeighbor / denominator;
}

// Neighbor i's contribution to the canonical sample's weight, to be summed over neighbors and added
// to the base term Mc / Mtot. `pHatFromNeighbor` is the neighbor's target at the canonical path
// shifted into the neighbor's domain, times that shift's Jacobian; 0 if the shift is undefined,
// which hands the whole pair to the canonical.
float pairwiseMisCanonicalTerm(const float Mc, const float Mi, const float N, const float Mtot, const float pHatCanonical, const float pHatFromNeighbor)
{
    const float denominator = Mc * pHatCanonical + N * pHatFromNeighbor;
    if (denominator <= 0.f)
    {
        return 0.f;
    }
    return (Mc / Mtot) * Mi * pHatCanonical / denominator;
}
