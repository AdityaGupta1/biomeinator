// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "oasis_shaping.h"
#include "biome_noise.h"
#include "terrain_formation.h"
#include "rendering/common/common_settings.h"
#include "util/rng.h"
#include "debug.h"

namespace OasisShaping
{
using namespace glm;
static uint32_t worldSeed;
constexpr float cellSize = 384.f;
constexpr float maxPondSupport = 115.f;
static_assert(2.f * maxPondSupport < 0.6f * cellSize);

void init(uint32_t seed)
{
    worldSeed = seed;
}

Context makeContext(ivec2 origin, ivec2 extent)
{
    Context result;
    result.minCell = ivec2(floor(vec2(origin) / cellSize)) - 1;
    const ivec2 maxCell = ivec2(floor(vec2(origin + extent) / cellSize)) + 1;
    result.size = maxCell - result.minCell + 1;
    result.ponds.reserve(result.size.x * result.size.y);
    for (int z = 0; z < result.size.y; ++z)
    {
        for (int x = 0; x < result.size.x; ++x)
        {
            const ivec2 cell = result.minCell + ivec2(x, z);
            RandomNumberGenerator rng = initRng(worldSeed ^ 0x0A515u, cell.x, cell.y);
            Pond pond{};
            pond.center = (vec2(cell) + vec2(rng.nextFloat(0.3f, 0.7f), rng.nextFloat(0.3f, 0.7f))) * cellSize;
            // A pond farther than its support from the context rect never shapes any sample,
            // so skip its noise evaluation.
            const vec2 nearestInContext = clamp(pond.center, vec2(origin), vec2(origin + extent));
            if (distance(nearestInContext, pond.center) >= maxPondSupport)
            {
                result.ponds.push_back(pond);
                continue;
            }
            pond.radius = rng.nextFloat(25.f, 37.f);
            const float angle = rng.nextFloat(0.f, 6.2831853f);
            pond.direction = vec2(cos(angle), sin(angle));
            pond.aspect = rng.nextFloat(1.03f, 1.12f);
            pond.shapeSeed = initRng(worldSeed ^ 0x5A0E1u, cell.x, cell.y).nextUint();
            RandomNumberGenerator shapeRng = initRng(pond.shapeSeed);
            const vec2 basinCenters[]{ { -0.6f, -0.15f }, { 0.15f, 0.f }, { 0.5f, 0.35f } };
            for (int i = 0; i < 3; ++i)
            {
                const vec2 center = basinCenters[i] + vec2(shapeRng.nextFloatAbs(0.05f), shapeRng.nextFloatAbs(0.05f));
                pond.basins[i] = vec3(center, shapeRng.nextFloat(0.70f - 0.12f * i, 0.78f - 0.09f * i));
            }
            pond.shoreNoiseBias = TerrainFormations::valueNoise2(vec2(0.f), pond.shapeSeed ^ 0x33u, pond.shapeSeed ^ 0xA9u);
            // Oases sit in flat, dry interior ground that no landform regime claims; shaping
            // a pond into terraces or spires would cut into their relief.
            const BiomeNoise n = BiomeNoiseFields::sampleAt(pond.center);
            pond.active = BiomeNoiseFields::dryClimateWeight(n) > 0.4f && BiomeNoiseFields::ruggedWeight(n) < 0.3f &&
                          n.inland > 0.32f && n.peak < 0.f && !BiomeNoiseFields::isClaimedByRegime(n);
            if (pond.active)
            {
                const auto terrain = BiomeNoiseFields::computeNaturalTerrain(n, pond.center);
                pond.level = max(SEA_LEVEL + 5, static_cast<int>(floor(terrain.formationBaseHeight)) - 3);
            }
            result.ponds.push_back(pond);
        }
    }
    return result;
}

Sample sample(vec2 pos, const Context& context)
{
    const ivec2 cell = ivec2(floor(pos / cellSize));
    for (int z = -1; z <= 1; ++z)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const ivec2 index = cell + ivec2(x, z) - context.minCell;
            ASSERT(index.x >= 0 && index.y >= 0 && index.x < context.size.x && index.y < context.size.y);
            const Pond& pond = context.ponds[index.x + context.size.x * index.y];
            if (!pond.active)
            {
                continue;
            }
            const vec2 worldDelta = pos - pond.center;
            if (dot(worldDelta, worldDelta) >= maxPondSupport * maxPondSupport)
            {
                continue;
            }
            const vec2 delta = worldDelta / pond.radius;
            const vec2 local(dot(delta, pond.direction), dot(delta, vec2(-pond.direction.y, pond.direction.x)));
            const vec2 stretched = local * vec2(1.f / pond.aspect, pond.aspect);
            const auto noise = [&](vec2 p, uint32_t salt)
            {
                return TerrainFormations::valueNoise(p, pond.shapeSeed ^ salt);
            };
            const vec2 warped = stretched +
                0.14f * TerrainFormations::valueNoise2(stretched * 1.3f, pond.shapeSeed ^ 0x14u, pond.shapeSeed ^ 0x71u);
            const auto distanceToBasin = [&](const vec3& basin) { return length(warped - vec2(basin)) - basin.z; };
            const auto join = [](float a, float b)
            {
                const float h = max(0.4f - abs(a - b), 0.f) / 0.4f;
                return min(a, b) - 0.1f * h * h;
            };
            // Overlapping basins make real inlets and narrow necks rather than merely
            // roughening an ellipse. The same distance drives the bowl, cutoff and banks.
            float radius = 1.f + join(join(distanceToBasin(pond.basins[0]), distanceToBasin(pond.basins[1])),
                                      distanceToBasin(pond.basins[2]));
            // Remove the site's noise bias: otherwise positive offsets shrink every basin
            // simultaneously into a group of little round pools instead of bending its shores.
            radius += 0.24f * clamp(noise(stretched * 1.4f, 0x33u) - pond.shoreNoiseBias.x, -1.f, 1.f) +
                      0.09f * clamp(noise(stretched * 4.2f, 0xA9u) - pond.shoreNoiseBias.y, -1.f, 1.f);
            if (radius >= 1.55f)
            {
                continue;
            }
            const float depth = 5.f + 2.f * noise(stretched * 1.7f, 0xD3u);
            const float bank = 4.5f + noise(stretched * 2.3f, 0xB4u);
            const float shelf = 0.5f + 0.22f * noise(stretched * 1.8f, 0x52u);
            // Support is bounded by (.7 + .78 + .55 + .33 + sqrt(2)*.14 + .2)*37*1.12 < 115 blocks,
            // below half the minimum 230.4-block site separation. The one-cell halo suffices.
            // The entire raised rim remains inside full shaping, before the outer blend.
            return {
                .weight = 1.f - smoothstep(1.25f, 1.55f, radius),
                .floorHeight = pond.level - depth + (depth + bank) * smoothstep(shelf, 1.15f, radius),
                .waterLevel = pond.level,
                .vegetation = radius < 1.45f + 0.18f * noise(stretched * 3.f, 0x6Eu),
                .wet = radius < 1.2f,
            };
        }
    }
    return {};
}
} // namespace OasisShaping
