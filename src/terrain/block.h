// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <cstdint>
#include <glm/glm.hpp>

using BlockId = uint16_t;

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
    DARKSHROOM,
    DARKSHROOM_STEM,
    DARKSHROOM_CAP,

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
};

namespace Blocks
{

void init();

const BlockData& getBlockData(Block block);

} // namespace Blocks
