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
    bool isDrift{ false };
};

class Decorator
{
private:
    std::vector<DecoratorEntry> entries{};
    float totalWeight{ 0.f };
    uint8_t unrestrictedSurfaces{ 0 };
    std::unordered_set<uint32_t> supportedSurfaceBlocks{};
    std::vector<Block> driftBlocks{};

public:
    void addEntry(Block block, float weight, std::initializer_list<Block> supportBlocks = {},
                  uint8_t surfaces = DECORATOR_SURFACE_FLOOR);
    // Drift entries share their combined weight, but each patch of ground (picked by driftSample)
    // grows only one of them, so e.g. meadow flowers come in single-species drifts. Floor only, and
    // all drift entries must share the same support blocks.
    void addDriftEntry(Block block, float weight, std::initializer_list<Block> supportBlocks = {});

    // driftSample is in [0, 1) and constant across a drift patch (see driftSample in chunk.cpp)
    Block getBlock(float rndSample, float driftSample, Block supportBlock, uint8_t surface) const;

    bool supportsSurface(uint8_t surface, Block supportBlock) const;

    bool isEmpty() const;

    const std::vector<DecoratorEntry>& getEntries() const;
};
