// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <cstdint>
#include <vector>

// A tileable texture pairing every texel with exactly one partner, so that paired spatial reuse can
// evaluate one shift per pair instead of two (ReSTIR PT Enhanced, Section 3). Each texel stores the
// wrapped coordinate delta to its partner; the partner stores the negated delta, so applying the
// pairing twice is the identity. Deltas roughly follow an isotropic Gaussian with the requested
// standard deviation.
struct PairingTexture
{
    struct Delta
    {
        int8_t x;
        int8_t y;
    };

    uint32_t size; // square, even, at most 254 so deltas fit in int8 after wrapping
    std::vector<Delta> deltas;

    Delta at(uint32_t x, uint32_t y) const
    {
        return deltas[y * size + x];
    }

    // Wrapped partner coordinate of a texel
    void partnerOf(uint32_t x, uint32_t y, uint32_t& partnerX, uint32_t& partnerY) const;
};

// Number of tiled 2x2 shuffles that yields the target delta standard deviation (Enhanced, Eq. 3)
uint32_t pairingShuffleCount(float sigma);

PairingTexture generatePairingTexture(uint32_t size, float sigma, uint32_t seed);
