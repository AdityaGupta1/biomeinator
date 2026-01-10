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

#include "terrain.h"

#include "chunk.h"
#include "logger.h"
#include "rendering/common/common_structs.h"
#include "rendering/buffer/to_free_list.h"
#include "scene/scene.h"

#include <stb_image.h>
#include <cstring>
#include <filesystem>
#include <vector>

namespace Terrain
{

static Scene* scene = nullptr;

static uint32_t loadTexture(const std::filesystem::path& path);

static uint32_t defaultMaterialIdx = MATERIAL_IDX_INVALID;
static void createMaterials();

void init(Scene* scene)
{
    Terrain::scene = scene;

    createMaterials();

    Blocks::init();
}

static uint32_t loadTexture(const std::filesystem::path& filename)
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

static void createMaterials()
{
    ToFreeList toFreeList{};

    const uint32_t diffuseTextureId = loadTexture("diffuse.png");
    if (diffuseTextureId == TEXTURE_ID_INVALID)
    {
        return;
    }

    const uint32_t emissionTextureId = loadTexture("emission.png");
    if (emissionTextureId == TEXTURE_ID_INVALID)
    {
        return;
    }

    Material material;
    material.emissiveStrength = 3.0f;
    material.baseColorTextureId = diffuseTextureId;
    material.emissiveColorTextureId = emissionTextureId;
    material.setHasDiffuse(true);

    defaultMaterialIdx = scene->addMaterial(toFreeList, &material);
}

} // namespace Terrain
