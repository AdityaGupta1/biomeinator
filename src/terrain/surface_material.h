// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once
#include "biome_noise.h"
#include "formation_rock.h"
#include "terrain_formation.h"
#include "rendering/common/common_settings.h"
#include "util/rng.h"
#include <limits>
#include <optional>

namespace SurfaceMaterials
{
using BiomeNoiseFields::TerrainRegime;

struct TerracottaLayer
{
    float bottom;
    Block base;
    Block accent; // AIR when the layer carries no seam
    float accentWidth;
    bool accentAtTop;
};

inline constexpr int terracottaLayerBlocks = 3;
inline constexpr float terracottaLayerThickness = static_cast<float>(terracottaLayerBlocks);
// Largest bedding displacement: strataVariation (+/-4) plus the Mesa bedding offset (+/-4).
inline constexpr int terracottaMaxOffset = 8;
// Covers every layer a lookup can touch: heights span [1 - maxOffset, chunkSizeY - 1 + maxOffset],
// and jitteredBand reads one boundary below and two above the unjittered guess.
inline constexpr int terracottaMinLayer = -(terracottaMaxOffset / terracottaLayerBlocks + 1) - 1;
inline constexpr int terracottaMaxLayer = (static_cast<int>(chunkSizeY) - 1 + terracottaMaxOffset) / terracottaLayerBlocks + 2;
inline constexpr int terracottaNumLayers = terracottaMaxLayer - terracottaMinLayer + 1;

// Seeded per world and built once at generator init, rather than hashed per voxel.
inline std::array<TerracottaLayer, terracottaNumLayers> terracottaLayers{};

inline void initTerracotta(uint32_t worldSeed)
{
    for (int i = 0; i < terracottaNumLayers; ++i)
    {
        const int layerIdx = terracottaMinLayer + i;
        TerracottaLayer& layer = terracottaLayers[i];
        RandomNumberGenerator boundaryRng = initRng(worldSeed ^ 0x57A7Au, layerIdx);
        layer.bottom = layerIdx * terracottaLayerThickness + boundaryRng.nextFloat(-0.9f, 0.9f);
        RandomNumberGenerator baseRng = initRng(worldSeed ^ 0xC1A7u, layerIdx);
        layer.base = baseRng.nextFloat() < 0.7f ? Block::TERRACOTTA : Block::ORANGE_TERRACOTTA;
        // Colored seams are 1-2 blocks thick within the earth-toned bedding. Either
        // edge can carry a seam, allowing adjacent accents as well as separated ones.
        auto accentRng = initRng(worldSeed ^ 0xACC31u, layerIdx);
        if (accentRng.nextFloat() >= 0.32f)
        {
            layer.accent = Block::AIR;
            continue;
        }
        layer.accentWidth = static_cast<float>(accentRng.nextInt(1, 3));
        layer.accentAtTop = accentRng.chance(0.5f);
        const float color = accentRng.nextFloat();
        if (color < 0.25f)
        {
            layer.accent = Block::RED_TERRACOTTA;
        }
        else if (color < 0.40f)
        {
            layer.accent = Block::YELLOW_TERRACOTTA;
        }
        else if (color < 0.70f)
        {
            layer.accent = Block::BROWN_TERRACOTTA;
        }
        else
        {
            layer.accent = Block::WHITE_TERRACOTTA;
        }
    }
}

inline const TerracottaLayer& terracottaLayer(int index)
{
    const int i = index - terracottaMinLayer;
    ASSERT(i >= 0 && i < terracottaNumLayers, "terracotta layer out of range");
    return terracottaLayers[i];
}

// Strata are keyed to absolute elevation, never column-top depth. Irregular boundaries
// and a nonrepeating palette sequence avoid identical stripes on every terrace.
inline Block terracotta(int y, float offset)
{
    const float height = static_cast<float>(y) + offset;
    const TerrainFormations::Band band = TerrainFormations::jitteredBand(
        height, static_cast<int>(glm::floor(height / terracottaLayerThickness)),
        [](int index) { return terracottaLayer(index).bottom; });
    const TerracottaLayer& layer = terracottaLayer(band.index);
    if (layer.accent == Block::AIR)
    {
        return layer.base;
    }
    const float width = glm::min(layer.accentWidth, band.high - band.low);
    const float depth = layer.accentAtTop ? band.high - height : height - band.low;
    return depth < width ? layer.accent : layer.base;
}

inline bool isQuartz(Block block)
{
    return block == Block::SMOOTH_QUARTZ;
}

inline bool isLedgeRock(Block block)
{
    return block == Block::STONE || FormationRock::isTianziRock(block);
}

// The supporting formation site owns its strata. Neighboring pillars get different
// elevations and sequences, while all columns/chunks of one pillar share its layers.
class TianziColumn
{
    struct Stratum
    {
        float top;
        Block block;
    };
    static constexpr size_t maxStrata = chunkSizeY / 20 + 4;
    std::array<Stratum, maxStrata> strata{};
    size_t layer = 0;
    float offset;
    glm::vec2 patchPos;
    float patchDetail;
    uint32_t patchSeed;
    int patchCell = std::numeric_limits<int>::min();
    float patchLow = 0.f;
    float patchHigh = 0.f;

    static float stratumDisplacement(glm::vec2 pos, uint32_t seed)
    {
        using namespace glm;
        // Offset whole pieces of the bedding across crooked fractures. Keeping the
        // same throw through the column preserves layer widths; smooth noise alone
        // makes the contacts look like level, painted stripes around each tower.
        const vec2 warped = pos + 7.f * TerrainFormations::valueNoise2(pos / 18.f, seed ^ 0xF271u, seed ^ 0xA731u) +
                            2.f * TerrainFormations::valueNoise2(pos / 4.f, seed ^ 0xC317u, seed ^ 0x195Bu);
        const vec2 p = warped / 20.f;
        float nearest = std::numeric_limits<float>::max();
        float faultThrow = 0.f;
        TerrainFormations::forEachNeighborCell(ivec2(floor(p)), seed ^ 0xFA017u, [&](ivec2 key, RandomNumberGenerator& rng)
        {
            const vec2 site = vec2(key) + vec2(rng.nextFloat(0.2f, 0.8f), rng.nextFloat(0.2f, 0.8f));
            const vec2 delta = p - site;
            const float distance = dot(delta, delta);
            if (distance < nearest)
            {
                nearest = distance;
                faultThrow = 6.8f * rng.nextInt(-3, 4);
            }
        });
        return faultThrow + 4.f * TerrainFormations::valueNoise(pos / 32.f, seed ^ 0x5721u) +
               1.5f * TerrainFormations::valueNoise(pos / 3.f, seed ^ 0x72A1u);
    }

public:
    TianziColumn(glm::vec2 pos, glm::ivec2 site, uint32_t seed)
    {
        auto rng = initRng(seed ^ 0x57A71u, site.x, site.y);
        int top = -40 + rng.nextInt(30);
        int color = rng.nextInt(3);
        constexpr auto& palette = FormationRock::tianziLayerBlocks;
        for (auto& stratum : strata)
        {
            top += rng.nextInt(20, 41);
            // Adjacent equal colors would merge into a visually over-thick band.
            color = (color + 1 + rng.nextInt(2)) % static_cast<int>(palette.size());
            stratum = { static_cast<float>(top), palette[color] };
        }
        offset = stratumDisplacement(pos, seed);
        const glm::vec2 warp = 6.f * TerrainFormations::valueNoise2(pos / 55.f, seed ^ 0x912u, seed ^ 0x713u);
        patchPos = (pos + warp) / 18.f;
        patchDetail = 0.08f * TerrainFormations::valueNoise(pos / 6.f, seed ^ 0xB41u);
        patchSeed = seed ^ 0xD47C1u;
    }

    Block rock(int y)
    {
        const float height = y + offset;
        while (layer > 0 && height < strata[layer - 1].top)
        {
            --layer;
        }
        while (layer + 1 < strata.size() && height >= strata[layer].top)
        {
            ++layer;
        }

        // A world-space 3D patch field crosses the strata rather than tinting whole
        // layers. Cache its XZ-interpolated planes; only Y interpolation is per voxel.
        const float patchY = y / 26.f;
        const int cell = static_cast<int>(glm::floor(patchY));
        if (cell != patchCell)
        {
            const auto plane = [&](int index)
            {
                return TerrainFormations::valueNoise(patchPos, patchSeed ^ hash(static_cast<uint32_t>(index)));
            };
            // The fill loop climbs one voxel at a time, so the previous upper plane is usually
            // this cell's lower one.
            patchLow = cell == patchCell + 1 ? patchHigh : plane(cell);
            patchHigh = plane(cell + 1);
            patchCell = cell;
        }
        const float f = glm::fract(patchY);
        const float patch = glm::mix(patchLow, patchHigh, f * f * (3.f - 2.f * f)) + patchDetail;
        return patch > 0.32f ? FormationRock::tianziPatchBlock : strata[layer].block;
    }
};

class Column
{
    const BiomeNoiseFields::NaturalTerrain& terrain;
    float variation;
    float mesaOffset;
    float tianziFloor;
    std::optional<TianziColumn> tianzi;

public:
    // Regime materials follow the unjittered coverage rather than the label, so per-column jitter
    // can't alternate materials along one cliff at a regime border.
    Column(const BiomeNoiseFields::NaturalTerrain& terrain, glm::vec2 pos, uint32_t seed, float variation,
           float tianziFloor)
        : terrain(terrain), variation(variation), mesaOffset(variation), tianziFloor(tianziFloor)
    {
        if (terrain.isCoveredBy(TerrainRegime::MESA))
        {
            // Displace the bedding together, preserving its thickness and the
            // existing deep-rock boundary. This does not move the terrain surface.
            mesaOffset += 3.f * TerrainFormations::valueNoise(pos / 24.f, seed ^ 0x6E51u) +
                          1.f * TerrainFormations::valueNoise(pos / 8.f, seed ^ 0xAB71u);
            ASSERT(glm::abs(mesaOffset) <= terracottaMaxOffset, "Mesa bedding offset exceeds the terracotta table");
        }
        if (terrain.isCoveredBy(TerrainRegime::TIANZI))
        {
            tianzi.emplace(pos, terrain.formationSite, seed);
        }
    }

    Block rock(int y)
    {
        // Quartz decides the carve mask and topsoil, so it follows the spire landform itself.
        if (terrain.regimeWeights[TerrainRegime::RED_DESERT] > 0.f &&
            terrain.formationHeight > 20.f && y > terrain.formationBaseHeight + 13.f + variation)
        {
            return Block::SMOOTH_QUARTZ;
        }
        if (terrain.isCoveredBy(TerrainRegime::MESA) && y > SEA_LEVEL - 14 + variation)
        {
            return terracotta(y, mesaOffset);
        }
        if (tianzi && y > tianziFloor)
        {
            return tianzi->rock(y);
        }
        if (terrain.isCoveredBy(TerrainRegime::RED_DESERT) && y > terrain.formationBaseHeight - 20.f)
        {
            return Block::RED_SANDSTONE;
        }
        return Block::AIR; // retain deep rock / cave biomes
    }
};
} // namespace SurfaceMaterials
