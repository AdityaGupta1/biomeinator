// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "unit_tests.h"

#include "rendering/restir/pairing_texture.h"

#include <cmath>
#include <cstdio>
#include <iostream>

// Fraction of an integer-valued N(0, sigma) sample with |x| <= bound, i.e. the continuous
// probability of |x| <= bound + 0.5
static double gaussianFractionWithin(const double bound, const double sigma)
{
    const double z = (bound + 0.5) / sigma;
    return std::erf(z / std::sqrt(2.0));
}

#define UNIT_ASSERT(cond)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        ++numAsserts;                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++numFailedAsserts;                                                                                        \
            std::cerr << "\033[31mASSERTION FAILED: " #cond "\n"                                                       \
                      << "  File: " << __FILE__ << ":" << __LINE__ << "\033[0m\n";                                     \
        }                                                                                                              \
    } while (0)

static bool testPairingTexture(const uint32_t size, const float sigma, const uint32_t seed)
{
    int numAsserts = 0;
    int numFailedAsserts = 0;

    const PairingTexture texture = generatePairingTexture(size, sigma, seed);
    const uint32_t texelCount = size * size;
    UNIT_ASSERT(texture.size == size);
    UNIT_ASSERT(texture.deltas.size() == texelCount);

    // Every texel has a partner that points back, and never itself
    bool allInvolutions = true;
    bool noSelfPairs = true;
    for (uint32_t y = 0; y < size; ++y)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            uint32_t px, py;
            texture.partnerOf(x, y, px, py);
            noSelfPairs &= !(px == x && py == y);
            uint32_t ppx, ppy;
            texture.partnerOf(px, py, ppx, ppy);
            allInvolutions &= (ppx == x && ppy == y);
        }
    }
    UNIT_ASSERT(allInvolutions);
    UNIT_ASSERT(noSelfPairs);

    // Deltas are wrapped to the short way round
    bool allWrapped = true;
    for (const PairingTexture::Delta delta : texture.deltas)
    {
        allWrapped &= std::abs(static_cast<int>(delta.x)) <= static_cast<int>(size / 2);
        allWrapped &= std::abs(static_cast<int>(delta.y)) <= static_cast<int>(size / 2);
    }
    UNIT_ASSERT(allWrapped);

    // Deltas are roughly an isotropic Gaussian with the requested standard deviation
    double sumX = 0, sumY = 0, sumX2 = 0, sumY2 = 0, sumXY = 0, sumRadial = 0;
    uint32_t withinOneSigma = 0;
    uint32_t withinTwoSigma = 0;
    for (const PairingTexture::Delta delta : texture.deltas)
    {
        const double dx = delta.x;
        const double dy = delta.y;
        sumX += dx;
        sumY += dy;
        sumX2 += dx * dx;
        sumY2 += dy * dy;
        sumXY += dx * dy;
        sumRadial += std::sqrt(dx * dx + dy * dy);
        withinOneSigma += (std::abs(dx) <= sigma) ? 1 : 0;
        withinTwoSigma += (std::abs(dx) <= 2.0 * sigma) ? 1 : 0;
    }
    const double meanX = sumX / texelCount;
    const double meanY = sumY / texelCount;
    const double stdX = std::sqrt(sumX2 / texelCount - meanX * meanX);
    const double stdY = std::sqrt(sumY2 / texelCount - meanY * meanY);
    const double correlation = (sumXY / texelCount - meanX * meanY) / (stdX * stdY);
    const double meanRadial = sumRadial / texelCount;
    const double expectedMeanRadial = sigma * std::sqrt(3.14159265358979 / 2.0);
    const double fracOneSigma = static_cast<double>(withinOneSigma) / texelCount;
    const double fracTwoSigma = static_cast<double>(withinTwoSigma) / texelCount;
    const double expectedFracOneSigma = gaussianFractionWithin(sigma, sigma);
    const double expectedFracTwoSigma = gaussianFractionWithin(2.0 * sigma, sigma);

    printf("  size %u sigma %.1f seed %u: shuffles %u, std (%.2f, %.2f), corr %.3f, mean radial %.2f (gaussian %.2f), "
           "within 1/2 sigma %.3f/%.3f (gaussian %.3f/%.3f)\n",
           size, sigma, seed, pairingShuffleCount(sigma), stdX, stdY, correlation, meanRadial, expectedMeanRadial,
           fracOneSigma, fracTwoSigma, expectedFracOneSigma, expectedFracTwoSigma);

    UNIT_ASSERT(std::abs(meanX) < 0.05 * sigma);
    UNIT_ASSERT(std::abs(meanY) < 0.05 * sigma);
    UNIT_ASSERT(std::abs(stdX - sigma) < 0.1 * sigma);
    UNIT_ASSERT(std::abs(stdY - sigma) < 0.1 * sigma);
    UNIT_ASSERT(std::abs(correlation) < 0.05);
    UNIT_ASSERT(std::abs(meanRadial - expectedMeanRadial) < 0.1 * expectedMeanRadial);
    UNIT_ASSERT(std::abs(fracOneSigma - expectedFracOneSigma) < 0.03);
    UNIT_ASSERT(std::abs(fracTwoSigma - expectedFracTwoSigma) < 0.02);

    if (numFailedAsserts == 0)
    {
        printf("\033[32m  All (%d) assertion(s) passed.\033[0m\n", numAsserts);
    }
    else
    {
        printf("\033[31m  %d/%d assertion(s) failed.\033[0m\n", numFailedAsserts, numAsserts);
    }
    return numFailedAsserts == 0;
}

bool runUnitTests()
{
    bool allPassed = true;

    printf("\n=============================================\n");
    printf("UNIT TEST: pairing_texture\n");
    printf("=============================================\n\n");
    // The three sizes and sigma used for paired spatial reuse, plus the smallest supported sigma
    allPassed &= testPairingTexture(254, 16.f, 1);
    allPassed &= testPairingTexture(230, 16.f, 2);
    allPassed &= testPairingTexture(210, 16.f, 3);
    allPassed &= testPairingTexture(64, 4.f, 4);

    return allPassed;
}
