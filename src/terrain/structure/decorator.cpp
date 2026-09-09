// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "decorator.h"

#include "debug.h"

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
    for (const DecoratorEntry& entry : this->entries)
    {
        if (entry.block != Block::AIR && (entry.surfaces & surface) &&
            (entry.supportBlocks.empty() || entry.supportBlocks.contains(supportBlock))) return true;
    }
    return false;
}

bool Decorator::isEmpty() const
{
    return this->entries.empty();
}
