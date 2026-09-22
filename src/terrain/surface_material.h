// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once
#include "biome_noise.h"
#include "terrain_formation.h"
#include "rendering/common/common_settings.h"
#include "util/rng.h"
#include <limits>
#include <optional>

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
    if (color < 0.40f) return Block::TERRACOTTA;
    if (color < 0.59f) return Block::ORANGE_TERRACOTTA;
    if (color < 0.76f) return Block::RED_TERRACOTTA;
    if (color < 0.85f) return Block::YELLOW_TERRACOTTA;
    // White and brown are thin accents, not broad repeated stripes.
    if (height - boundary(layer) > 1.5f) return Block::TERRACOTTA;
    return color < 0.93f ? Block::BROWN_TERRACOTTA : Block::WHITE_TERRACOTTA;
}

inline bool isQuartz(Block block)
{
    return block == Block::SMOOTH_QUARTZ || block == Block::IVORY_QUARTZ;
}

inline bool isTianziStone(Block block)
{
    return block == Block::GRAY_SANDSTONE || block == Block::BUFF_SANDSTONE ||
           block == Block::WEATHERED_SANDSTONE || block == Block::DARK_SANDSTONE;
}

inline bool isLedgeRock(Block block)
{
    return block == Block::STONE || isTianziStone(block);
}

// The supporting formation site owns its strata. Neighboring pillars get different
// elevations and sequences, while all columns/chunks of one pillar share its layers.
class TianziColumn
{
    struct Stratum { float top; Block block; };
    static constexpr size_t maxStrata = chunkSizeY / 20 + 4;
    std::array<Stratum, maxStrata> strata{};
    size_t layer = 0;
    float offset;
    glm::vec2 patchPos;
    float patchDetail;
    uint32_t patchSeed;
    int patchCell = std::numeric_limits<int>::min();
    float patchLow = 0.f, patchHigh = 0.f;

    static float stratumDisplacement(glm::vec2 pos, uint32_t seed)
    {
        using namespace glm;
        // Offset whole pieces of the bedding across crooked fractures. Keeping the
        // same throw through the column preserves layer widths; smooth noise alone
        // made the contacts look like level, painted stripes around each tower.
        const vec2 warped = pos + 7.f * vec2(
            TerrainFormations::valueNoise(pos / 18.f, seed ^ 0xF271u),
            TerrainFormations::valueNoise(pos / 18.f, seed ^ 0xA731u)) + 2.f * vec2(
            TerrainFormations::valueNoise(pos / 4.f, seed ^ 0xC317u),
            TerrainFormations::valueNoise(pos / 4.f, seed ^ 0x195Bu));
        const vec2 p = warped / 20.f;
        const ivec2 cell = ivec2(floor(p));
        float nearest = std::numeric_limits<float>::max();
        float faultThrow = 0.f;
        for (int z = -1; z <= 1; ++z)
        for (int x = -1; x <= 1; ++x)
        {
            const ivec2 key = cell + ivec2(x, z);
            auto rng = initRng(seed ^ 0xFA017u, key.x, key.y);
            const vec2 site = vec2(key) + vec2(rng.nextFloat(0.2f, 0.8f), rng.nextFloat(0.2f, 0.8f));
            const vec2 delta = p - site;
            const float distance = dot(delta, delta);
            if (distance < nearest)
            {
                nearest = distance;
                faultThrow = 6.8f * rng.nextInt(-3, 4);
            }
        }
        return faultThrow + 4.f * TerrainFormations::valueNoise(pos / 32.f, seed ^ 0x5721u) +
               1.5f * TerrainFormations::valueNoise(pos / 3.f, seed ^ 0x72A1u);
    }

public:
    TianziColumn(glm::vec2 pos, glm::ivec2 site, uint32_t seed)
    {
        auto rng = initRng(seed ^ 0x57A71u, site.x, site.y);
        int top = -40 + rng.nextInt(30);
        int color = rng.nextInt(3);
        constexpr std::array palette{ Block::GRAY_SANDSTONE, Block::BUFF_SANDSTONE,
                                      Block::WEATHERED_SANDSTONE };
        for (auto& stratum : strata)
        {
            top += rng.nextInt(20, 41);
            // Adjacent equal colors would merge into a visually over-thick band.
            color = (color + 1 + rng.nextInt(2)) % static_cast<int>(palette.size());
            stratum = { static_cast<float>(top), palette[color] };
        }
        offset = stratumDisplacement(pos, seed);
        const glm::vec2 warp = 6.f * glm::vec2(
            TerrainFormations::valueNoise(pos / 55.f, seed ^ 0x912u),
            TerrainFormations::valueNoise(pos / 55.f, seed ^ 0x713u));
        patchPos = (pos + warp) / 18.f;
        patchDetail = 0.08f * TerrainFormations::valueNoise(pos / 6.f, seed ^ 0xB41u);
        patchSeed = seed ^ 0xD47C1u;
    }

    Block rock(int y)
    {
        const float height = y + offset;
        while (layer > 0 && height < strata[layer - 1].top) --layer;
        while (layer + 1 < strata.size() && height >= strata[layer].top) ++layer;

        // A world-space 3D patch field crosses the strata rather than tinting whole
        // layers. Cache its XZ-interpolated planes; only Y interpolation is per voxel.
        const float patchY = y / 26.f;
        const int cell = static_cast<int>(glm::floor(patchY));
        if (cell != patchCell)
        {
            const auto plane = [&](int index) {
                return TerrainFormations::valueNoise(patchPos, patchSeed ^ hash(static_cast<uint32_t>(index)));
            };
            patchLow = plane(cell);
            patchHigh = plane(cell + 1);
            patchCell = cell;
        }
        const float f = glm::fract(patchY);
        const float patch = glm::mix(patchLow, patchHigh, f * f * (3.f - 2.f * f)) + patchDetail;
        return patch > 0.32f ? Block::DARK_SANDSTONE : strata[layer].block;
    }
};

class Column
{
    Biome biome;
    BiomeNoiseFields::NaturalTerrain terrain;
    float variation;
    float mesaOffset;
    float tianziFloor;
    Block quartzMaterial = Block::SMOOTH_QUARTZ;
    std::optional<TianziColumn> tianzi;

public:
    Column(Biome biome, const BiomeNoiseFields::NaturalTerrain& terrain, glm::vec2 pos,
           uint32_t seed, float variation, bool formationRock, float tianziFloor)
        : biome(biome), terrain(terrain), variation(variation), mesaOffset(variation), tianziFloor(tianziFloor)
    {
        if (biome == Biome::MESA)
        {
            // Displace the bedding together, preserving its thickness and the
            // existing deep-rock boundary. This does not move the terrain surface.
            mesaOffset += 3.f * TerrainFormations::valueNoise(pos / 24.f, seed ^ 0x6E51u) +
                          1.f * TerrainFormations::valueNoise(pos / 8.f, seed ^ 0xAB71u);
        }
        if (formationRock) tianzi.emplace(pos, terrain.formationSite, seed);
        if (biome == Biome::RED_DESERT && terrain.formationHeight > 20.f)
        {
            // Ivory encroaches radially on a smooth core; the taper of the spike
            // exposes the core at its tip without any elevation-based color cutoff.
            const float coreRadius = 0.6f +
                0.12f * TerrainFormations::valueNoise(pos / 7.f, seed ^ 0xC7A2u) +
                0.05f * TerrainFormations::valueNoise(pos / 2.5f, seed ^ 0xB451u);
            quartzMaterial = terrain.quartzRadiusFraction > coreRadius ? Block::IVORY_QUARTZ : Block::SMOOTH_QUARTZ;
        }
    }

    Block rock(int y)
    {
        if (biome == Biome::MESA && y > SEA_LEVEL - 14 + variation)
            return terracotta(y, mesaOffset);
        if (tianzi && y > tianziFloor)
            return tianzi->rock(y);
        if (biome == Biome::RED_DESERT && y > terrain.formationBaseHeight - 20.f)
        {
            if (terrain.formationHeight > 20.f && y > terrain.formationBaseHeight + 13.f + variation)
                return quartzMaterial;
            return Block::RED_SANDSTONE;
        }
        return Block::AIR; // retain deep rock / cave biomes
    }
};
} // namespace SurfaceMaterials
