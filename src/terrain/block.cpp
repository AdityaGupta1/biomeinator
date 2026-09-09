// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "block.h"
#include "block_model.h"

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
    { "glass", BlockType::GLASS },
};

static const std::unordered_map<std::string, BlockShape> blockShapesByName = {
    { "cube", BlockShape::CUBE },
    { "x_shaped", BlockShape::X_SHAPED },
    { "liquid_top", BlockShape::LIQUID_TOP },
    { "decorator_custom", BlockShape::DECORATOR_CUSTOM },
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

BlockData readBlockJson(const std::filesystem::path& jsonPath)
{
    std::ifstream file(jsonPath);
    if (!file)
    {
        Logger::logError("blocks: failed to open %s", jsonPath.generic_string().c_str());
        return {};
    }

    BlockData data;
    nlohmann::json blockJson;
    try
    {
        blockJson = nlohmann::json::parse(file);

        if (blockJson.contains("shape"))
        {
            data.shape = parseNamedValue(blockShapesByName, blockJson["shape"], "shape");
        }


        if (blockJson.contains("textures"))
        {
            const nlohmann::json& texturesJson = blockJson["textures"];
            if (texturesJson.is_string())
            {
                data.texSlices = BlockTexSlices(resolveTextureSlice(texturesJson.get<std::string>()));
            }
            else
            {
                const uint32_t top = resolveTextureSlice(texturesJson.at("top").get<std::string>());
                const uint32_t side = resolveTextureSlice(texturesJson.at("side").get<std::string>());
                const uint32_t bottom = resolveTextureSlice(texturesJson.at("bottom").get<std::string>());
                data.texSlices = BlockTexSlices(top, side, bottom);
            }
        }

        if (blockJson.contains("type"))
        {
            data.type = parseNamedValue(blockTypesByName, blockJson["type"], "type");
        }

        data.markAsEmitter = blockJson.value("markAsEmitter", false);
        data.translucent = blockJson.value("translucent", false);
        data.proceduralColor = blockJson.value("proceduralColor", false);
        if (data.shape == BlockShape::DECORATOR_CUSTOM)
        {
            if (!blockJson.at("textures").is_string() || data.texSlices[0] == TEX_SLICE_INVALID)
                throw std::runtime_error("custom decorators require one texture atlas");
            const std::filesystem::path name = blockJson.at("model").get<std::string>();
            if (name.empty() || name.has_parent_path() || name.extension() != ".glb")
                throw std::runtime_error("model must be a GLB filename in assets/blocks/models");
            if (data.type != BlockType::SOLID && data.type != BlockType::TRANSPARENT_CUTOUT)
                throw std::runtime_error("custom decorators must use solid or transparent_cutout type");
            if (blockJson.contains("randomRotationY"))
            {
                const auto& turns = blockJson.at("randomRotationY");
                if (!turns.is_array() || turns.empty() || turns.size() > 4)
                    throw std::runtime_error("randomRotationY must contain 1 to 4 distinct quarter-turn angles");
                unsigned seen = 0;
                data.numRotationsY = static_cast<uint8_t>(turns.size());
                for (size_t i = 0; i < turns.size(); ++i)
                {
                    if (!turns[i].is_number_integer()) throw std::runtime_error("rotation angles must be integers");
                    const int degrees = turns[i].get<int>();
                    if (degrees < 0 || degrees > 270 || degrees % 90 != 0 || (seen & (1u << (degrees / 90))))
                        throw std::runtime_error("rotation angles must be distinct values from 0, 90, 180, 270");
                    seen |= 1u << (degrees / 90);
                    data.rotationY[i] = static_cast<uint8_t>(degrees / 90);
                }
            }
            data.modelIdx = BlockModels::load(jsonPath.parent_path() / "models" / name);
        }
    }
    catch (const std::exception& e)
    {
        Logger::logError("blocks: failed to parse %s: %s", jsonPath.generic_string().c_str(), e.what());
        // Never silently turn a malformed model into a full occluding cube.
        if (data.shape == BlockShape::DECORATOR_CUSTOM) throw;
        return {};
    }
    return data;
}

void init()
{
    namespace fs = std::filesystem;

    const fs::path blocksDir = fs::path(TARGET_FILE_DIR) / "assets/blocks";
    BlockModels::clear();
    blockDatas.fill(BlockData{});
    blocksById.clear();
    textureNames.clear();
    sliceByTextureName.clear();
    blocksById.reserve(blockIdNames.size());
    for (size_t i = 0; i < blockIdNames.size(); ++i)
    {
        blocksById.emplace(blockIdNames[i], static_cast<Block>(i));
        const fs::path jsonPath = blocksDir / (std::string(blockIdNames[i]) + ".json");
        blockDatas[i] = readBlockJson(jsonPath);
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
