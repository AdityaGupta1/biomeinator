// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "decorator.h"

#include "../terrain_formation.h"
#include "debug.h"
#include "util/rng.h"

namespace
{

uint32_t surfaceBlockKey(uint8_t surface, Block block)
{
    return (static_cast<uint32_t>(surface) << 16) | static_cast<uint32_t>(block);
}

// One uniform value per patch of ground about 20 blocks across, which picks that patch's drift
// species. Noise-warped cell borders keep the patches from reading as a grid.
float driftSample(glm::ivec2 posXZ_WS, uint32_t worldSeed)
{
    const uint32_t seed = worldSeed ^ hash(640921733);
    const glm::vec2 pos = glm::vec2(posXZ_WS);
    const glm::vec2 warpedPos =
        pos + 10.f * TerrainFormations::valueNoise2(pos / 24.f, seed ^ 0x51Du, seed ^ 0x2A7u);
    const glm::ivec2 cell = glm::ivec2(glm::floor(warpedPos / 20.f));
    return initRng(seed, static_cast<uint32_t>(cell.x), static_cast<uint32_t>(cell.y)).nextFloat();
}

} // namespace

void Decorator::addEntry(Block block, float weight, std::initializer_list<Block> supportBlocks, uint8_t surfaces)
{
    ASSERT(weight > 0.f);
    ASSERT((surfaces & ~DECORATOR_SURFACE_ALL) == 0 && surfaces != 0);
    this->entries.push_back({
        block,
        weight,
        std::unordered_set<Block>(supportBlocks),
        surfaces,
    });
    if (block != Block::AIR)
    {
        for (const uint8_t surface : { DECORATOR_SURFACE_FLOOR, DECORATOR_SURFACE_WALL,
                                      DECORATOR_SURFACE_CEILING })
        {
            if (!(surfaces & surface)) continue;
            if (supportBlocks.size() == 0)
            {
                this->unrestrictedSurfaces |= surface;
            }
            else
            {
                for (const Block supportBlock : supportBlocks)
                    this->supportedSurfaceBlocks.insert(surfaceBlockKey(surface, supportBlock));
            }
        }
    }
    totalWeight += weight;
}

void Decorator::addDriftEntry(Block block, float weight, std::initializer_list<Block> supportBlocks)
{
    this->addDriftEntry({ block }, weight, supportBlocks);
}

void Decorator::addDriftEntry(std::initializer_list<Block> blocks, float weight, std::initializer_list<Block> supportBlocks)
{
    ASSERT(blocks.size() > 0);
    this->addEntry(*blocks.begin(), weight, supportBlocks);
    DecoratorEntry& entry = this->entries.back();
    entry.isDrift = true;
    entry.driftBlocks = blocks;
    this->driftTotalWeight += weight;
}

Block Decorator::getBlock(
    float rndSample, glm::ivec2 posXZ_WS, uint32_t worldSeed, Block supportBlock, uint8_t surface) const
{
    if (this->isEmpty())
    {
        ASSERT(false, "empty decorators should not be called");
        return Block::AIR;
    }

    rndSample *= this->totalWeight;
    int entryIdx = 0;
    const int maxEntryIdx = this->entries.size() - 1;
    while (entryIdx < maxEntryIdx)
    {
        rndSample -= this->entries[entryIdx].weight;
        if (rndSample < 0.f)
        {
            break;
        }
        entryIdx++;
    }

    ASSERT(entryIdx >= 0 && entryIdx < this->entries.size());

    const DecoratorEntry& entry = this->entries[entryIdx];
    const bool supportBlockValid = entry.supportBlocks.empty() || entry.supportBlocks.contains(supportBlock);
    if (!supportBlockValid || !(entry.surfaces & surface))
    {
        return Block::AIR;
    }
    if (entry.isDrift)
    {
        float driftWeightSample = driftSample(posXZ_WS, worldSeed) * this->driftTotalWeight;
        const DecoratorEntry* driftEntry = nullptr;
        for (const DecoratorEntry& candidate : this->entries)
        {
            if (!candidate.isDrift)
            {
                continue;
            }
            driftEntry = &candidate;
            driftWeightSample -= candidate.weight;
            if (driftWeightSample < 0.f)
            {
                break;
            }
        }
        const std::vector<Block>& driftBlocks = driftEntry->driftBlocks;
        if (driftBlocks.size() == 1)
        {
            return driftBlocks[0];
        }
        RandomNumberGenerator variantRng = initRng(worldSeed ^ hash(214733061),
            static_cast<uint32_t>(posXZ_WS.x), static_cast<uint32_t>(posXZ_WS.y /*z*/));
        return driftBlocks[variantRng.nextUint() % driftBlocks.size()];
    }
    return entry.block;
}

bool Decorator::supportsSurface(uint8_t surface, Block supportBlock) const
{
    ASSERT(surface == DECORATOR_SURFACE_FLOOR || surface == DECORATOR_SURFACE_WALL ||
           surface == DECORATOR_SURFACE_CEILING);
    return (this->unrestrictedSurfaces & surface) ||
           this->supportedSurfaceBlocks.contains(surfaceBlockKey(surface, supportBlock));
}

bool Decorator::isEmpty() const
{
    return this->entries.empty();
}

const std::vector<DecoratorEntry>& Decorator::getEntries() const
{
    return this->entries;
}
