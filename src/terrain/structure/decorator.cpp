// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "decorator.h"

#include "debug.h"

void Decorator::addEntry(Block block, float weight, std::initializer_list<Block> groundBlocks)
{
    ASSERT(weight > 0.f);
    this->entries.push_back({
        block,
        weight,
        std::unordered_set<Block>(groundBlocks),
    });
    totalWeight += weight;
}

Block Decorator::getBlock(float rndSample, Block bottomBlock) const
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
    const bool groundBlockValid = entry.groundBlocks.empty() || entry.groundBlocks.contains(bottomBlock);
    return groundBlockValid ? entry.block : Block::AIR;
}

bool Decorator::isEmpty() const
{
    return this->entries.empty();
}
