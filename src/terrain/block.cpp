// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "block.h"

#include "logger.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <string>
#include <unordered_map>

using namespace glm;

BlockUvs::BlockUvs(uvec2 all)
    : BlockUvs(all, all, all)
{}

BlockUvs::BlockUvs(uvec2 top, uvec2 side, uvec2 bottom)
    : uvs{ side, top, bottom }
{}

const glm::uvec2& BlockUvs::operator[](uint32_t idx) const
{
    return this->uvs[idx];
}

namespace Blocks
{

std::array<BlockData, static_cast<size_t>(Block::COUNT)> blockDatas;

static std::unordered_map<std::string_view, Block> blocksById;

static const std::unordered_map<std::string, BlockType> blockTypesByName = {
    { "air", BlockType::AIR },
    { "water", BlockType::WATER },
    { "solid", BlockType::SOLID },
    { "transparent_cutout", BlockType::TRANSPARENT_CUTOUT },
};

static const std::unordered_map<std::string, BlockShape> blockShapesByName = {
    { "cube", BlockShape::CUBE },
    { "x_shaped", BlockShape::X_SHAPED },
    { "liquid_top", BlockShape::LIQUID_TOP },
};

static uvec2 parseUv(const nlohmann::json& uvJson)
{
    return uvec2(uvJson.at(0).get<uint32_t>(), uvJson.at(1).get<uint32_t>());
}

static bool parseBlockJson(const std::filesystem::path& jsonPath, BlockData& outData)
{
    std::ifstream file(jsonPath);
    if (!file)
    {
        Logger::logError("blocks: failed to open %s", jsonPath.generic_string().c_str());
        return false;
    }

    nlohmann::json blockJson;
    try
    {
        blockJson = nlohmann::json::parse(file);

        if (blockJson.contains("uvs"))
        {
            const nlohmann::json& uvsJson = blockJson["uvs"];
            if (uvsJson.is_array())
            {
                outData.uvs = BlockUvs(parseUv(uvsJson));
            }
            else
            {
                outData.uvs = BlockUvs(parseUv(uvsJson.at("top")),
                                       parseUv(uvsJson.at("side")),
                                       parseUv(uvsJson.at("bottom")));
            }
        }

        if (blockJson.contains("type"))
        {
            outData.type = blockTypesByName.at(blockJson["type"].get<std::string>());
        }

        if (blockJson.contains("shape"))
        {
            outData.shape = blockShapesByName.at(blockJson["shape"].get<std::string>());
        }

        outData.emitsLight = blockJson.value("emitsLight", false);
        outData.translucent = blockJson.value("translucent", false);
    }
    catch (const std::exception& e)
    {
        Logger::logError("blocks: failed to parse %s: %s", jsonPath.generic_string().c_str(), e.what());
        return false;
    }

    return true;
}

void init()
{
    namespace fs = std::filesystem;

    blocksById.reserve(blockIdNames.size());
    for (size_t i = 0; i < blockIdNames.size(); ++i)
    {
        blocksById.emplace(blockIdNames[i], static_cast<Block>(i));
    }

    const fs::path blocksDir = fs::path(TARGET_FILE_DIR) / "assets/blocks";
    for (size_t i = 0; i < blockIdNames.size(); ++i)
    {
        const fs::path jsonPath = blocksDir / (std::string(blockIdNames[i]) + ".json");
        parseBlockJson(jsonPath, blockDatas[i]);
    }
}

const BlockData& getBlockData(Block block)
{
    return blockDatas[static_cast<size_t>(block)];
}

Block fromId(std::string_view id)
{
    const auto it = blocksById.find(id);
    return it != blocksById.end() ? it->second : Block::COUNT;
}

} // namespace Blocks
