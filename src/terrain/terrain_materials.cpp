// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "terrain_materials.h"
#include "terrain_materials_helpers.h"
#include "terrain_omm.h"

#include "rendering/buffer/to_free_list.h"
#include "rendering/renderer.h"

namespace TerrainMaterials
{

static void createMaterials(Scene* scene);

void init(Scene* scene)
{
    createMaterials(scene);
}

static std::array<uint32_t, static_cast<size_t>(TerrainMaterial::COUNT)> materialIdxs;

// Aux map g channel is the biome tint mask; per-slice presence drives TRIANGLE_FLAG_BIOME_TINT
static std::vector<bool> sliceBiomeTintMask;

#define MATERIAL_IDX(material) materialIdxs[static_cast<size_t>(material)]

static void createMaterials(Scene* scene)
{
    ToFreeList toFreeList{};

    std::vector<uint8_t> diffuseAlpha;
    const uint32_t diffuseTextureId = loadTexture(scene, "diffuse.png", { .outAlphaChannel = &diffuseAlpha });
    if (diffuseTextureId == TEXTURE_ID_INVALID)
    {
        return;
    }

    if (Renderer::getUseOmms())
    {
        TerrainOmm::bake(diffuseAlpha, TERRAIN_TEXTURE_SIZE, TERRAIN_TILE_SIZE);
    }

    const uint32_t auxTextureId = loadTexture(scene, "aux_map.png",
                                              { .sRGB = false,
                                                .outTileHasBiomeTintMask = &sliceBiomeTintMask,
                                                .alphaOverride = &diffuseAlpha });
    if (auxTextureId == TEXTURE_ID_INVALID)
    {
        return;
    }

    {
        Material defaultMaterial{};
        defaultMaterial.emissiveStrength = 3.0f;
        defaultMaterial.baseColorTextureId = diffuseTextureId;
        defaultMaterial.auxTextureId = auxTextureId;
        defaultMaterial.setHasDiffuse(true);
        defaultMaterial.setHasArrayTexture(true);
        defaultMaterial.setHasPackedAux(true);
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

bool sliceHasBiomeTint(uint32_t sliceIdx)
{
    return sliceIdx < sliceBiomeTintMask.size() && sliceBiomeTintMask[sliceIdx];
}

} // namespace TerrainMaterials
