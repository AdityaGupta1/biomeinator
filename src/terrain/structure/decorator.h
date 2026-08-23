// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../block.h"

#include <unordered_set>
#include <vector>

struct DecoratorEntry
{
    Block block{ Block::AIR };
    float weight{ 1.f };
    std::unordered_set<Block> groundBlocks{};
    Block topBlock{ Block::AIR }; // placed directly above block, for two-tall plants
    bool needsAdjacentWater{ false }; // ground block must have water horizontally adjacent
};

class Decorator
{
private:
    std::vector<DecoratorEntry> entries{};
    float totalWeight{ 0.f };

public:
    void addEntry(DecoratorEntry entry);
    void addEntry(Block block, float weight, std::initializer_list<Block> groundBlocks = {});

    const DecoratorEntry* getEntry(float rndSample, Block bottomBlock) const;

    bool isEmpty() const;
};
