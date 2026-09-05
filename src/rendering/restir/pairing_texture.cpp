// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "pairing_texture.h"

#include "util/rng.h"

#include <cassert>
#include <cmath>
#include <utility>

void PairingTexture::partnerOf(uint32_t x, uint32_t y, uint32_t& partnerX, uint32_t& partnerY) const
{
    const Delta delta = at(x, y);
    partnerX = (x + size + delta.x) % size;
    partnerY = (y + size + delta.y) % size;
}

uint32_t pairingShuffleCount(float sigma)
{
    const float count = sigma * sigma / 2.f + 1.46f / sigma + 1.76f / (sigma * sigma) + 0.656f / (sigma * sigma * sigma) + 0.5f;
    return static_cast<uint32_t>(std::floor(count));
}

// Wraps a coordinate delta into [-size/2, size/2) so links stay short across the tiling seam
static int wrapDelta(int delta, int size)
{
    const int half = size / 2;
    if (delta >= half)
    {
        return delta - size;
    }
    if (delta < -half)
    {
        return delta + size;
    }
    return delta;
}

PairingTexture generatePairingTexture(uint32_t size, float sigma, uint32_t seed)
{
    assert(size % 2 == 0 && size >= 2 && size <= 254);

    const uint32_t texelCount = size * size;

    // Link indices: every index appears exactly twice, on horizontally adjacent texels
    std::vector<uint32_t> links(texelCount);
    for (uint32_t i = 0; i < texelCount; ++i)
    {
        links[i] = i / 2;
    }

    // Repeated 2x2 shuffles move each link index in a random walk; alternating the block grid's
    // diagonal offset lets indices cross block borders
    RandomNumberGenerator rng = initRng(seed);
    const uint32_t shuffleCount = pairingShuffleCount(sigma);
    for (uint32_t shuffleIdx = 0; shuffleIdx < shuffleCount; ++shuffleIdx)
    {
        const uint32_t gridOffset = shuffleIdx % 2;
        for (uint32_t blockY = 0; blockY < size; blockY += 2)
        {
            for (uint32_t blockX = 0; blockX < size; blockX += 2)
            {
                uint32_t cells[4];
                for (uint32_t cellIdx = 0; cellIdx < 4; ++cellIdx)
                {
                    const uint32_t x = (blockX + gridOffset + (cellIdx & 1)) % size;
                    const uint32_t y = (blockY + gridOffset + (cellIdx >> 1)) % size;
                    cells[cellIdx] = y * size + x;
                }

                // Fisher-Yates over the four cells
                for (uint32_t i = 3; i > 0; --i)
                {
                    const uint32_t j = static_cast<uint32_t>(rng.nextInt(static_cast<int>(i) + 1));
                    std::swap(links[cells[i]], links[cells[j]]);
                }
            }
        }
    }

    // Each link index names two texels; find them
    constexpr uint32_t unset = ~0u;
    std::vector<uint32_t> firstTexelOfLink(texelCount / 2, unset);
    std::vector<uint32_t> partnerOfTexel(texelCount, unset);
    for (uint32_t texelIdx = 0; texelIdx < texelCount; ++texelIdx)
    {
        uint32_t& first = firstTexelOfLink[links[texelIdx]];
        if (first == unset)
        {
            first = texelIdx;
        }
        else
        {
            partnerOfTexel[first] = texelIdx;
            partnerOfTexel[texelIdx] = first;
        }
    }

    PairingTexture texture;
    texture.size = size;
    texture.deltas.resize(texelCount);
    const int sizeInt = static_cast<int>(size);
    for (uint32_t texelIdx = 0; texelIdx < texelCount; ++texelIdx)
    {
        const uint32_t partnerIdx = partnerOfTexel[texelIdx];
        assert(partnerIdx != unset);
        const int dx = wrapDelta(static_cast<int>(partnerIdx % size) - static_cast<int>(texelIdx % size), sizeInt);
        const int dy = wrapDelta(static_cast<int>(partnerIdx / size) - static_cast<int>(texelIdx / size), sizeInt);
        texture.deltas[texelIdx] = { static_cast<int8_t>(dx), static_cast<int8_t>(dy) };
    }
    return texture;
}
