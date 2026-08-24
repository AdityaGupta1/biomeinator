// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "block.h"

#include "logger.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <stdexcept>
#include <unordered_map>

BlockTexSlices::BlockTexSlices(uint32_t all)
    : BlockTexSlices(all, all, all)
{}

BlockTexSlices::BlockTexSlices(uint32_t top, uint32_t side, uint32_t bottom)
    : slices{ side, top, bottom }
{}

uint32_t BlockTexSlices::operator[](uint32_t idx) const
{
    return this->slices[idx];
}

namespace Blocks
{

std::array<BlockData, static_cast<size_t>(Block::COUNT)> blockDatas;

static std::unordered_map<std::string_view, Block> blocksById;

static std::vector<std::string> textureNames;
static std::unordered_map<std::string, uint32_t> sliceByTextureName;

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

// Slices are assigned in first-reference order
static uint32_t resolveTextureSlice(const std::string& textureName)
{
    const auto [it, inserted] = sliceByTextureName.try_emplace(textureName,
                                                              static_cast<uint32_t>(textureNames.size()));
    if (inserted)
    {
        textureNames.push_back(textureName);
    }
    return it->second;
}

template <typename T>
static T parseNamedValue(const std::unordered_map<std::string, T>& valuesByName,
                         const nlohmann::json& nameJson,
                         const char* fieldName)
{
    const std::string name = nameJson.get<std::string>();
    const auto it = valuesByName.find(name);
    if (it == valuesByName.end())
    {
        throw std::runtime_error("unknown " + std::string(fieldName) + " '" + name + "'");
    }
    return it->second;
}

static void parseBlockJson(const std::filesystem::path& jsonPath, BlockData& outData)
{
    std::ifstream file(jsonPath);
    if (!file)
    {
        Logger::logError("blocks: failed to open %s", jsonPath.generic_string().c_str());
        return;
    }

    nlohmann::json blockJson;
    try
    {
        blockJson = nlohmann::json::parse(file);

        if (blockJson.contains("textures"))
        {
            const nlohmann::json& texturesJson = blockJson["textures"];
            if (texturesJson.is_string())
            {
                outData.texSlices = BlockTexSlices(resolveTextureSlice(texturesJson.get<std::string>()));
            }
            else
            {
                const uint32_t top = resolveTextureSlice(texturesJson.at("top").get<std::string>());
                const uint32_t side = resolveTextureSlice(texturesJson.at("side").get<std::string>());
                const uint32_t bottom = resolveTextureSlice(texturesJson.at("bottom").get<std::string>());
                outData.texSlices = BlockTexSlices(top, side, bottom);
            }
        }

        if (blockJson.contains("type"))
        {
            outData.type = parseNamedValue(blockTypesByName, blockJson["type"], "type");
        }

        if (blockJson.contains("shape"))
        {
            outData.shape = parseNamedValue(blockShapesByName, blockJson["shape"], "shape");
        }

        outData.emitsLight = blockJson.value("emitsLight", false);
        outData.translucent = blockJson.value("translucent", false);
    }
    catch (const std::exception& e)
    {
        Logger::logError("blocks: failed to parse %s: %s", jsonPath.generic_string().c_str(), e.what());
    }
}

void init()
{
    namespace fs = std::filesystem;

    const fs::path blocksDir = fs::path(TARGET_FILE_DIR) / "assets/blocks";
    blocksById.reserve(blockIdNames.size());
    for (size_t i = 0; i < blockIdNames.size(); ++i)
    {
        blocksById.emplace(blockIdNames[i], static_cast<Block>(i));
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

const std::vector<std::string>& getTextureNames()
{
    return textureNames;
}

} // namespace Blocks
