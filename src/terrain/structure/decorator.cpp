/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2026 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

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
    int entryIdx = -1;
    while (rndSample > 0.f)
    {
        rndSample -= this->entries[++entryIdx].weight;
    }

    const DecoratorEntry& entry = this->entries[entryIdx];
    const bool groundBlockValid = entry.groundBlocks.empty() || entry.groundBlocks.contains(bottomBlock);
    return groundBlockValid ? entry.block : Block::AIR;
}

bool Decorator::isEmpty() const
{
    return this->entries.empty();
}
