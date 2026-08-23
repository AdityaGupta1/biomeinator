// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <cstdint>
#include <glm/glm.hpp>

using BlockId = uint16_t;

// Serialized by value in world exports — only append new types.
enum class Block : BlockId
{
    AIR = 0,

    WATER,
    WATER_TOP,

    BEDROCK,
    STONE,
    LAMP,
    DIRT,
    GRASS_BLOCK,
    SAND,
    SANDSTONE,
    SNOW,
    SNOWY_GRASS_BLOCK,
    ICE,
    OAK_LOG,
    OAK_LEAVES,
    CACTUS,
    GRASS,
    SHORT_GRASS,
    DEAD_BUSH,
    DEAD_GRASS_1,
    DEAD_GRASS_2,
    GOLDENROD,
    TINY_CACTUS,
    PINK_DAFFODIL,
    BLACK_SAND,
    GRAVEL,
    PALM_LOG,
    PALM_LEAVES,
    ACACIA_LOG,
    ACACIA_LEAVES,
    LAVA,
    LAVA_TOP,
    MARBLE,
    SCALESTONE,
    HELLSTONE,
    MOSS,
    RAINBOW_CRYSTAL,
    CHERRY_LOG,
    CHERRY_LEAVES_PINK,
    CHERRY_LEAVES_WHITE,
    REDWOOD_LOG,
    REDWOOD_LEAVES,
    BIRCH_LOG,
    BIRCH_LEAVES_GREEN,
    BIRCH_LEAVES_YELLOW,
    BIRCH_LEAVES_ORANGE,
    FIR_LOG,
    FIR_LEAVES,
    PINE_LOG,
    PINE_LEAVES,
    COBBLESTONE,
    MOSSY_COBBLESTONE,
    MAHOGANY_LOG,
    MAHOGANY_LEAVES,
    EUCALYPTUS_LOG,
    EUCALYPTUS_LEAVES,
    WILLOW_LOG,
    WILLOW_LEAVES,
    CYPRESS_LOG,
    CYPRESS_LEAVES,
    MUD,
    SPANISH_MOSS,
    SPANISH_MOSS_TIP,
    BROWN_MUSHROOM,
    BLUE_ORCHID,
    CATTAIL,
    CATTAIL_TIP,

    COUNT
};

enum class BlockType : uint8_t
{
    AIR,
    WATER,
    SOLID,
    TRANSPARENT_CUTOUT,

    COUNT
};

enum class BlockShape : uint8_t
{
    CUBE,
    X_SHAPED,
    LIQUID_TOP,

    COUNT
};

struct BlockUvs
{
private:
    glm::uvec2 uvs[3]; // integer position in block texture grid
                       // order = side, top, bottom

public:
    BlockUvs() = default;
    BlockUvs(glm::uvec2 all);
    BlockUvs(glm::uvec2 top, glm::uvec2 side, glm::uvec2 bottom);

    const glm::uvec2& operator[](uint32_t idx) const;
};

struct BlockData
{
    BlockUvs uvs{};
    BlockType type{ BlockType::SOLID };
    BlockShape shape{ BlockShape::CUBE };
    bool emitsLight{ false };
    bool translucent{ false }; // thin diffuse transmission (leaves and living foliage)
};

namespace Blocks
{

void init();

const BlockData& getBlockData(Block block);

} // namespace Blocks
