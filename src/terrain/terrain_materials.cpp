// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "terrain_materials.h"
#include "terrain_materials_helpers.h"
#include "terrain_omm.h"

#include "block.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/renderer.h"
#include <stdexcept>

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

    const bool useOmms = Renderer::getUseOmms();

    const std::vector<std::string>& textureNames = Blocks::getTextureNames();
    ASSERT(!textureNames.empty());

    std::vector<std::vector<uint8_t>> diffuseAlphas;
    const uint32_t diffuseTextureId = loadBlockTextureArray(
        scene, textureNames, "", { .outAlphaChannels = &diffuseAlphas, .useOpaqueCutoutMips = useOmms });
    if (diffuseTextureId == TEXTURE_ID_INVALID)
    {
        return;
    }

    // Validate even on pre-OMM hardware: arbitrary model UVs only support opaque tiles.
    for (size_t i = 0; i < static_cast<size_t>(Block::COUNT); ++i)
    {
        const auto& block = Blocks::getBlockData(static_cast<Block>(i));
        if (block.shape != BlockShape::DECORATOR_CUSTOM) continue;
        const auto slice = block.texSlices[0];
        if (slice >= diffuseAlphas.size() || diffuseAlphas[slice].empty() ||
            std::any_of(diffuseAlphas[slice].begin(), diffuseAlphas[slice].end(), [](uint8_t a) { return a != 255; }))
            throw std::runtime_error("custom decorator texture must be fully opaque: " + textureNames.at(slice));
    }

    if (useOmms)
    {
        TerrainOmm::bake(diffuseAlphas, TERRAIN_TILE_SIZE);
    }

    const uint32_t auxTextureId = loadBlockTextureArray(scene, textureNames, ".aux",
                                                        { .sRGB = false,
                                                          .outSliceHasBiomeTintMask = &sliceBiomeTintMask,
                                                          .alphaOverrides = &diffuseAlphas,
                                                          .useOpaqueCutoutMips = useOmms,
                                                          .missingFilesAreZero = true });
    if (auxTextureId == TEXTURE_ID_INVALID)
    {
        return;
    }

    {
        Material defaultMaterial{};
        defaultMaterial.emissiveStrength = 5.0f;
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
