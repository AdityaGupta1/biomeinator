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
#include "terrain_materials_helpers.h"

#include "rendering/buffer/to_free_list.h"

#include <array>

namespace TerrainMaterials
{

static void createMaterials(Scene* scene);

void init(Scene* scene)
{
    createMaterials(scene);
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
        waterMaterial.baseColor = { 1.f, 1.f, 1.f }; // blue color comes from volume absorption
        waterMaterial.setHasDiffuse(false);
        waterMaterial.setHasGlossyReflection(true);
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
