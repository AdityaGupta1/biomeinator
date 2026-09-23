// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "util/rng.h"
#include "debug.h"
#include <array>
#include <glm/glm.hpp>
#include <limits>

namespace TerrainFormations
{

inline float valueNoise(glm::vec2 pos, uint32_t seed)
{
    using namespace glm;
    const ivec2 cell = ivec2(floor(pos));
    const vec2 f = fract(pos);
    const vec2 t = f * f * (3.f - 2.f * f);
    const auto at = [&](int x, int z)
    {
        RandomNumberGenerator rng = initRng(seed, cell.x + x, cell.y + z);
        return rng.nextFloat(-1.f, 1.f);
    };
    return mix(mix(at(0, 0), at(1, 0), t.x), mix(at(0, 1), at(1, 1), t.x), t.y);
}

// smoothstep with its [edge0, edge1] window stretched about the midpoint by widen (1 = unchanged).
// Softer versions of a ramp keep the same center, so they stay centered on the same boundary.
inline float widenedSmoothstep(float edge0, float edge1, float x, float widen = 1.f)
{
    const float mid = 0.5f * (edge0 + edge1);
    const float halfWidth = 0.5f * (edge1 - edge0) * widen;
    return glm::smoothstep(mid - halfWidth, mid + halfWidth, x);
}

// Smooth window: rises from 0 to 1 over [riseStart, riseEnd], then falls back to 0 over
// [fallStart, fallEnd], each ramp widened by widen.
inline float smoothBand(float x, float riseStart, float riseEnd, float fallStart, float fallEnd, float widen = 1.f)
{
    return widenedSmoothstep(riseStart, riseEnd, x, widen) * (1.f - widenedSmoothstep(fallStart, fallEnd, x, widen));
}

// Two independent value noise channels at one position, e.g. for a 2D domain warp.
inline glm::vec2 valueNoise2(glm::vec2 pos, uint32_t seedX, uint32_t seedY)
{
    return { valueNoise(pos, seedX), valueNoise(pos, seedY) };
}

// Visits the cells within `radius` cells of cell (3x3 by default), each with its own
// position-seeded random stream.
template<typename Visit>
inline void forEachNeighborCell(glm::ivec2 cell, uint32_t seed, const Visit& visit, int radius = 1)
{
    for (int z = -radius; z <= radius; ++z)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            const glm::ivec2 key = cell + glm::ivec2(x, z);
            RandomNumberGenerator rng = initRng(seed, key.x, key.y);
            visit(key, rng);
        }
    }
}

struct Band
{
    int index;
    float low;
    float high;
};

// Corrects an unjittered band guess to the band whose jittered lower boundary lies at or below
// value, returning that band's boundaries. One step suffices while each boundary(i) stays within
// half a band of its regular spot.
template<typename Boundary>
inline Band jitteredBand(float value, int guess, const Boundary& boundary)
{
    const float low = boundary(guess);
    if (low > value)
    {
        return { guess - 1, boundary(guess - 1), low };
    }
    const float high = boundary(guess + 1);
    if (high < value)
    {
        return { guess + 1, high, boundary(guess + 2) };
    }
    return { guess, low, high };
}

// A connected plateau field with broken escarpments and short gullies. Unlike the
// isolated-site profile below, its outlines come from warped, overlapping scales.
inline float plateauRelief(glm::vec2 pos, uint32_t seed)
{
    using namespace glm;
    pos += 42.f * valueNoise2(pos / 210.f, seed ^ 0x713u, seed ^ 0x951u);
    const float broad = valueNoise(pos / 170.f, seed);
    const float medium = valueNoise(pos / 65.f, seed ^ 0x823u);
    const float detail = valueNoise(pos / 22.f, seed ^ 0x195u);
    const float plateau = smoothstep(-0.25f, 0.3f, broad * 0.7f + medium * 0.3f + detail * 0.08f);
    const float gullies = 1.f - smoothstep(0.02f, 0.16f, abs(medium + detail * 0.18f));
    return plateau - gullies * (0.07f + plateau * 0.15f) + detail * 0.035f;
}

// Shared finite-support landform, independent of biomes, chunks and voxel resolution. Broad
// feet and narrower cores use the same sites, so a spire always grows out of its own foothill.
struct Profile
{
    float spacing;
    float radius;
    float footRadius;
    float height;
    float footHeight;
    float summitWidth;
    float angularity{ 0.f }; // round hills at 0, faceted cores with accelerating roots at 1
};

inline float sample(glm::vec2 pos, uint32_t seed, const Profile& profile, glm::ivec2* dominantSite = nullptr)
{
    using namespace glm;
    // An omitted site is at least 1.25 cells away. Keep even stretched/warped support
    // inside that distance so moving the 3x3 search window cannot introduce a seam.
    ASSERT(profile.spacing > 0.f && profile.radius > 0.f && profile.footRadius > 0.f);
    ASSERT(profile.summitWidth >= 0.f && profile.summitWidth < 1.f);
    ASSERT(profile.angularity >= 0.f && profile.angularity <= 1.f);
    const float warpBound = profile.radius * 0.3f * 1.415f + 3.f;
    const float facetBound = mix(1.f, 1.083f, profile.angularity);
    ASSERT(max(profile.footRadius, profile.radius * 1.2f) * facetBound / 0.8f + warpBound < 1.25f * profile.spacing);
    const ivec2 cell = ivec2(floor(pos / profile.spacing));
    const vec2 warped = pos + profile.radius * 0.3f *
        valueNoise2(pos / profile.radius, seed ^ 0x541u, seed ^ 0x901u);
    const float summitRoughness = valueNoise(pos / (profile.radius * 0.8f), seed ^ 0x339u) *
                                  profile.summitWidth * profile.height * 0.11f;
    struct Site
    {
        float distance;
        float radius;
        float height;
    };
    const auto evaluateSite = [&](ivec2 key, RandomNumberGenerator& rng) -> Site
    {
        const vec2 site = (vec2(key) + vec2(rng.nextFloat(0.25f, 0.75f), rng.nextFloat(0.25f, 0.75f))) * profile.spacing;
        const float radius = profile.radius * rng.nextFloat(0.75f, 1.2f);
        const float height = profile.height * rng.nextFloat(0.65f, 1.25f);
        const vec2 stretch(rng.nextFloat(0.8f, 1.2f), rng.nextFloat(0.8f, 1.2f));
        // Small continuous distortion roughens footprints without adding another noise grid.
        vec2 delta = warped - site;
        delta += 2.f * vec2(sin(pos.y * 0.17f + site.x), sin(pos.x * 0.19f + site.y));
        delta *= stretch;
        float distance = length(delta);
        if (profile.angularity > 0.f)
        {
            const float angle = rng.nextFloat(0.f, 6.2831853f);
            const vec2 rotated = abs(vec2(delta.x * cos(angle) - delta.y * sin(angle),
                                          delta.x * sin(angle) + delta.y * cos(angle)));
            const float faceted = max(max(rotated.x, rotated.y), (rotated.x + rotated.y) * 0.7071068f);
            distance = mix(distance, faceted, profile.angularity);
        }
        return { distance, radius, height };
    };

    float result = 0.f;
    ivec2 supportingSite{};
    forEachNeighborCell(cell, seed, [&](ivec2 key, RandomNumberGenerator& rng)
    {
        const Site site = evaluateSite(key, rng);
        // Both the foot and the core are zero beyond their radii.
        if (site.distance >= max(profile.footRadius, site.radius))
        {
            return;
        }
        const float inward = max(0.f, 1.f - site.distance / profile.footRadius);
        // Subtract the linear term so the outer edge meets the desert with zero slope.
        // The exponential then steepens toward the crystal instead of rounding into a dome.
        const float risingFoot = (exp(3.f * inward) - 1.f - 3.f * inward) / (exp(3.f) - 4.f);
        const float foot = mix(1.f - smoothstep(0.f, profile.footRadius, site.distance), risingFoot, profile.angularity);
        const float core = mix(1.f - smoothstep(profile.summitWidth, 1.f, site.distance / site.radius),
            clamp((1.f - site.distance / site.radius) / (1.f - profile.summitWidth), 0.f, 1.f), profile.angularity);
        const float contribution = profile.footHeight * foot + (site.height + summitRoughness) * core;
        if (contribution > result)
        {
            result = contribution;
            supportingSite = key;
        }
    });

    if (dominantSite)
    {
        *dominantSite = supportingSite;
        if (result <= 0.f)
        {
            // Between feet no site contributes. Falling back to the nearest site keeps ownership
            // continuous there. Unlike support, the nearest distorted site can lie outside the
            // 3x3 window (stretch and warp can make a far site measure closer), so search 5x5:
            // omitted sites are then at least 2.25 cells away, beyond any in-window distortion.
            float nearestDistance = std::numeric_limits<float>::max();
            forEachNeighborCell(cell, seed, [&](ivec2 key, RandomNumberGenerator& rng)
            {
                const float distance = evaluateSite(key, rng).distance;
                if (distance < nearestDistance)
                {
                    nearestDistance = distance;
                    *dominantSite = key;
                }
            }, 2);
        }
    }
    return result;
}

// Smaller independent fields sit on the shoulders below. Gate each tier by its
// immediate support, so upper crowns cannot rise out of valleys or skip a tier.
// This stays a pure height query for distant terrain and other stacked karst profiles.
template<size_t N>
inline float sampleStacked(glm::vec2 pos, uint32_t seed, const std::array<Profile, N>& tiers,
                          glm::ivec2* foundationSite = nullptr)
{
    static_assert(N > 0);
    float previous = sample(pos, seed, tiers[0], foundationSite);
    float height = previous;
    float support = 1.f;
    for (size_t i = 1; i < N; ++i)
    {
        support *= glm::smoothstep(tiers[i - 1].height * 0.35f, tiers[i - 1].height * 0.7f, previous);
        if (support <= 0.f)
        {
            break;
        }
        previous = sample(pos, seed ^ (0xA271u * static_cast<uint32_t>(i)), tiers[i]);
        height += support * previous;
    }
    return height;
}

} // namespace TerrainFormations
