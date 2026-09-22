// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once
#include "biome_noise.h"
#include "terrain_formation.h"
#include "rendering/common/common_settings.h"
#include "util/rng.h"

namespace SurfaceMaterials
{
// Strata are keyed to absolute elevation, never column-top depth. Irregular boundaries
// and a nonrepeating palette sequence avoid identical stripes on every terrace.
inline Block terracotta(int y, float offset)
{
    const float height = static_cast<float>(y) + offset;
    int layer = static_cast<int>(glm::floor(height / 3.f));
    const auto boundary = [](int index)
    {
        RandomNumberGenerator rng = initRng(0x57A7Au, index);
        return index * 3.f + rng.nextFloat(-0.9f, 0.9f);
    };
    if (boundary(layer) > height) --layer;
    else if (boundary(layer + 1) < height) ++layer;
    RandomNumberGenerator rng = initRng(0xC1A7u, layer);
    const float color = rng.nextFloat();
    if (color < 0.28f) return Block::TERRACOTTA;
    if (color < 0.50f) return Block::ORANGE_TERRACOTTA;
    if (color < 0.70f) return Block::RED_TERRACOTTA;
    if (color < 0.80f) return Block::YELLOW_TERRACOTTA;
    // White and brown are thin accents, not broad repeated stripes.
    if (height - boundary(layer) > 1.5f) return Block::TERRACOTTA;
    return color < 0.91f ? Block::BROWN_TERRACOTTA : Block::WHITE_TERRACOTTA;
}

// Correlated formation-rock coverage leaves existing stone/marble outcrops at the
// roots. It follows the same continuous influence as geometry, not a jittered label.
inline bool tianziRock(const BiomeNoise& noise, glm::vec2 pos, uint32_t seed)
{
    const float coverage = glm::smoothstep(0.10f, 0.65f, BiomeNoiseFields::tianziWeight(noise));
    const float patch = 0.5f + 0.49f * TerrainFormations::valueNoise(pos / 28.f, seed ^ 0x5A7D57u);
    return coverage > patch;
}

inline Block rock(Biome biome, int y, const BiomeNoiseFields::NaturalTerrain& terrain, float variation,
                  bool formationRock)
{
    if (biome == Biome::MESA && y > SEA_LEVEL - 14 + variation)
        return terracotta(y, variation);
    if (formationRock && y > terrain.formationBaseHeight - 22.f)
        return Block::STONE;
    if (biome == Biome::RED_DESERT && y > terrain.formationBaseHeight - 20.f)
    {
        if (terrain.formationHeight > 20.f && y > terrain.formationBaseHeight + 13.f + variation)
            return Block::QUARTZ;
        return Block::RED_SANDSTONE;
    }
    return Block::AIR; // retain the ordinary rock / cave biome
}
} // namespace SurfaceMaterials
