// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "terrain/block_model.h"
#include "terrain/chunk.h"
#include "terrain/terrain.h"
#include "terrain/terrain_materials.h"
#include "terrain/terrain_omm.h"
#include "rendering/buffer/to_free_list.h"
#include "settings_manager.h"

#include <stdexcept>

namespace
{
[[noreturn]] void unexpectedRendererCall()
{
    throw std::logic_error("CPU terrain tests called a rendering path");
}
}

// CPU tests use the real block JSON loader and generator. Geometry loading and
// renderer scheduling are outside these tests; generation only needs block metadata.
namespace BlockModels
{
void clear() {}
uint32_t load(const std::filesystem::path&, bool) { return 0; }
const Model& get(uint32_t) { unexpectedRendererCall(); }
const std::vector<Vertex>& Model::getOrientation(uint8_t, uint8_t) const { unexpectedRendererCall(); }
}

uint32_t regionTestSeed = 96;
namespace SettingsManager
{
uint32_t getWorldSeed() { return regionTestSeed; }
}

namespace Terrain
{
void addChunkToRevisit(Chunk*) {}
void addChunkToCreateBlas(Chunk*) { unexpectedRendererCall(); }
void addChunkToDestroy(Chunk*) { unexpectedRendererCall(); }
}

namespace TerrainMaterials
{
uint32_t getMaterialIdx(TerrainMaterial) { unexpectedRendererCall(); }
bool sliceHasBiomeTint(uint32_t) { unexpectedRendererCall(); }
bool sliceHasNormalMap(uint32_t) { unexpectedRendererCall(); }
}

namespace TerrainOmm
{
bool isBaked() { unexpectedRendererCall(); }
bool texArraySliceHasCutout(uint32_t) { unexpectedRendererCall(); }
uint16_t getOmmIdx(uint32_t, uint32_t) { unexpectedRendererCall(); }
}

PerFaceData::PerFaceData() { unexpectedRendererCall(); }
void PerFaceData::setFlags(uint32_t) { unexpectedRendererCall(); }
void PerFaceData::setTexArraySliceIdx(uint32_t) { unexpectedRendererCall(); }
void ToFreeList::pushInstance(Instance*) { unexpectedRendererCall(); }
void Instance::setTransformOffset(glm::ivec3) { unexpectedRendererCall(); }
void Instance::finalizeGeometry() { unexpectedRendererCall(); }
void Instance::addAreaLights(const std::vector<uint32_t>&) { unexpectedRendererCall(); }
bool Instance::getIsGeometryFinalized() const { unexpectedRendererCall(); }
void Instance::setVisible(bool) { unexpectedRendererCall(); }
void Instance::setMaterialIdx(uint32_t) { unexpectedRendererCall(); }
void Instance::setIsDeformable(bool) { unexpectedRendererCall(); }
void Instance::setIsOpaque(bool) { unexpectedRendererCall(); }
void Instance::setTrisPerFaceLog2(uint32_t) { unexpectedRendererCall(); }
