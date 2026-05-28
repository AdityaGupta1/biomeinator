// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "terrain_materials.h"
#include "terrain_materials_helpers.h"

#include "rendering/buffer/to_free_list.h"

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
        defaultMaterial.emissiveStrength = 5.0f;
        defaultMaterial.baseColorTextureId = diffuseTextureId;
        defaultMaterial.emissiveColorTextureId = emissionTextureId;
        defaultMaterial.setHasDiffuse(true);
        defaultMaterial.setHasArrayTexture(true);
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
