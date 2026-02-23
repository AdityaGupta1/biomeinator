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

#include "terrain_materials.h"

#include "logger.h"
#include "rendering/common/common_structs.h"
#include "rendering/buffer/to_free_list.h"
#include "scene/scene.h"

#include <stb_image.h>
#include <cstring>
#include <filesystem>
#include <vector>

namespace TerrainMaterials
{

static uint32_t loadTexture(Scene* scene, const std::filesystem::path& path);

static void createMaterials(Scene* scene);

void init(Scene* scene)
{
    createMaterials(scene);
}

static uint32_t loadTexture(Scene* scene, const std::filesystem::path& filename)
{
    namespace fs = std::filesystem;

    const fs::path fullPath = fs::path(TARGET_FILE_DIR) / fs::path("assets/textures/") / filename;

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* data = stbi_load(fullPath.generic_string().c_str(), &width, &height, &channels, 4);

    if (data == nullptr)
    {
        Logger::logError("Failed to load texture from: %s", fullPath.generic_string().c_str());
        return TEXTURE_ID_INVALID;
    }

    const size_t size = static_cast<size_t>(width) * height * 4;
    std::vector<uint8_t> textureData(size);
    std::memcpy(textureData.data(), data, size);
    stbi_image_free(data);

    return scene->addTexture(std::move(textureData), static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

static std::array<uint32_t, static_cast<size_t>(TerrainMaterial::COUNT)> materialIdxs;

#define MATERIAL_IDX(material) materialIdxs[static_cast<size_t>(material)]

static void createMaterials(Scene* scene)
{
    ToFreeList toFreeList{};

    const uint32_t diffuseTextureId = loadTexture(scene, "diffuse.png");
    if (diffuseTextureId == TEXTURE_ID_INVALID)
    {
        return;
    }

    const uint32_t emissionTextureId = loadTexture(scene, "emission.png");
    if (emissionTextureId == TEXTURE_ID_INVALID)
    {
        return;
    }

    {
        Material defaultMaterial{};
        defaultMaterial.emissiveStrength = 3.0f;
        defaultMaterial.baseColorTextureId = diffuseTextureId;
        defaultMaterial.emissiveColorTextureId = emissionTextureId;
        defaultMaterial.setHasDiffuse(true);
        MATERIAL_IDX(TerrainMaterial::DEFAULT) = scene->addMaterial(toFreeList, &defaultMaterial);
    }

    {
        Material waterMaterial{};
        waterMaterial.baseColor = { 29.f / 255.f, 162.f / 255.f, 216.f / 255.f }; // TODO: replace with volume absorption
        waterMaterial.setHasDiffuse(false);
        waterMaterial.setHasGlossyTransmission(true);
        waterMaterial.ior = 1.33f;
        MATERIAL_IDX(TerrainMaterial::WATER) = scene->addMaterial(toFreeList, &waterMaterial);
    }

    toFreeList.freeAll();
}

uint32_t getMaterialIdx(TerrainMaterial terrainMaterial)
{
    return MATERIAL_IDX(terrainMaterial);
}

} // namespace TerrainMaterials
