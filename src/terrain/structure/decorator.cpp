// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "decorator.h"

#include "debug.h"

namespace
{

uint32_t surfaceBlockKey(uint8_t surface, Block block)
{
    return (static_cast<uint32_t>(surface) << 16) | static_cast<uint32_t>(block);
}

} // namespace

void Decorator::addEntry(Block block, float weight, std::initializer_list<Block> supportBlocks, uint8_t surfaces)
{
    ASSERT(weight > 0.f);
    ASSERT((surfaces & ~DECORATOR_SURFACE_ALL) == 0 && surfaces != 0);
    if (block != Block::AIR && (surfaces & (DECORATOR_SURFACE_WALL | DECORATOR_SURFACE_CEILING)))
    {
        const BlockData& blockData = Blocks::getBlockData(block);
        ASSERT(blockData.shape == BlockShape::DECORATOR_CUSTOM &&
               blockData.stateKind == BlockStateKind::SURFACE_MOUNT,
               "wall/ceiling decorators require a surface-mounted custom model");
    }
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

Block Decorator::getBlock(float rndSample, Block supportBlock, uint8_t surface) const
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
    return supportBlockValid && (entry.surfaces & surface) ? entry.block : Block::AIR;
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
