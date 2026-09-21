// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "util/rng.h"
#include "debug.h"
#include <glm/glm.hpp>

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

// A connected plateau field with broken escarpments and short gullies. Unlike the
// isolated-site profile below, its outlines come from warped, overlapping scales.
inline float plateauRelief(glm::vec2 pos, uint32_t seed)
{
    using namespace glm;
    pos += 42.f * vec2(valueNoise(pos / 210.f, seed ^ 0x713u),
                      valueNoise(pos / 210.f, seed ^ 0x951u));
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

inline float sample(glm::vec2 pos, uint32_t seed, const Profile& profile)
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
    const vec2 warped = pos + profile.radius * 0.3f * vec2(
        valueNoise(pos / profile.radius, seed ^ 0x541u), valueNoise(pos / profile.radius, seed ^ 0x901u));
    const float summitRoughness = valueNoise(pos / (profile.radius * 0.8f), seed ^ 0x339u) *
                                  profile.summitWidth * profile.height * 0.11f;
    float result = 0.f;
    for (int z = -1; z <= 1; ++z)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const ivec2 key = cell + ivec2(x, z);
            RandomNumberGenerator rng = initRng(seed, key.x, key.y);
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
            const float inward = max(0.f, 1.f - distance / profile.footRadius);
            // Subtract the linear term so the outer edge meets the desert with zero slope.
            // The exponential then steepens toward the crystal instead of rounding into a dome.
            const float risingFoot = (exp(3.f * inward) - 1.f - 3.f * inward) / (exp(3.f) - 4.f);
            const float foot = mix(1.f - smoothstep(0.f, profile.footRadius, distance), risingFoot, profile.angularity);
            const float core = mix(1.f - smoothstep(profile.summitWidth, 1.f, distance / radius),
                clamp((1.f - distance / radius) / (1.f - profile.summitWidth), 0.f, 1.f), profile.angularity);
            result = max(result, profile.footHeight * foot + (height + summitRoughness) * core);
        }
    }
    return result;
}

// A smaller independent field sits on the shoulders of the broad field. Gate its
// contribution by support height, so secondary crowns cannot rise out of valleys.
// This stays a pure height query for distant terrain and other stacked karst profiles.
inline float sampleStacked(glm::vec2 pos, uint32_t seed, const Profile& lower, const Profile& upper)
{
    const float base = sample(pos, seed, lower);
    const float support = glm::smoothstep(lower.height * 0.18f, lower.height * 0.6f, base);
    return base + support * sample(pos, seed ^ 0xA271u, upper);
}

} // namespace TerrainFormations
