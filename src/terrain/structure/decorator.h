// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../block.h"

#include <unordered_set>
#include <vector>

enum DecoratorSurface : uint8_t
{
    DECORATOR_SURFACE_FLOOR = 1u << 0,
    DECORATOR_SURFACE_WALL = 1u << 1,
    DECORATOR_SURFACE_CEILING = 1u << 2,
    DECORATOR_SURFACE_ALL = DECORATOR_SURFACE_FLOOR | DECORATOR_SURFACE_WALL | DECORATOR_SURFACE_CEILING,
};

struct DecoratorEntry
{
    Block block{ Block::AIR };
    float weight{ 1.f };
    std::unordered_set<Block> supportBlocks{};
    uint8_t surfaces{ DECORATOR_SURFACE_FLOOR };
};

class Decorator
{
private:
    std::vector<DecoratorEntry> entries{};
    float totalWeight{ 0.f };

public:
    void addEntry(Block block, float weight, std::initializer_list<Block> supportBlocks = {},
                  uint8_t surfaces = DECORATOR_SURFACE_FLOOR);

    Block getBlock(float rndSample, Block supportBlock, uint8_t surface) const;

    bool supportsSurface(uint8_t surface, Block supportBlock) const;

    bool isEmpty() const;
};
