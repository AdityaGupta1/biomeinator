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
};

class Decorator
{
private:
    std::vector<DecoratorEntry> entries{};
    float totalWeight{ 0.f };

public:
    void addEntry(Block block, float weight, std::initializer_list<Block> groundBlocks = {});

    Block getBlock(float rndSample, Block bottomBlock) const;

    bool isEmpty() const;
};
