// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "terrain_lod.h"

#include "biome.h"
#include "block.h"
#include "block_orientation.h"
#include "chunk.h"
#include "chunk_generator.h"
#include "surface_chunk_cache.h"
#include "terrain_materials.h"
#include "multithreading/thread_memory_allocator.h"
#include "multithreading/thread_pool.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/cpu_profiler.h"
#include "scene/scene.h"
#include "util/glm_util.h"
#include "util/packing.h"
#include "util/rng.h"

#include <algorithm>
#include <bit>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace glm;

// A tile at level L covers 2^L x 2^L chunks with at most 2^maxCellsPerSideLog2 cells per side, so cells
// are single blocks up to the level where that many cells span the tile and double in size every
// level above it
inline constexpr int maxCellsPerSideLog2 = 8;
// A tile is replaced by its children within this many of its own widths of the camera, so a cell spans
// about the same angle wherever its level is shown
inline constexpr int subdivideDistanceTiles = 2;
// Tiles up to this level are voxel tiles where voxel tiles are on: surface-only chunks with their
// structures, downsampled (see SurfaceChunkCache). Tiles within the voxel distance are subdivided down to
// this level, so structures and 3D landforms continue past the chunk distance.
inline constexpr int maxVoxelTileLevel = 2;
// Tiles entirely within the chunk distance are only placeholders until their chunks are ready. Below
// this level there are too many of them to be worth generating, so their parents stand in. Voxel tiles
// would cost as much as the chunks they stand in for.
inline constexpr int minPlaceholderLevel = maxVoxelTileLevel + 1;
// Tiles within this many chunks of the chunk distance's edge keep their geometry even where chunks
// cover them: moving away needs them as soon as the chunks leave, sooner than they could be generated
inline constexpr int keepGeometryMarginChunks = 4;
// Skirts reach this many cells below a tile's edges
inline constexpr int edgeSkirtDepthCells = 4;
// Steeper cells show their slope's material rather than their top block: block terrain this steep shows
// as much side as top
inline constexpr float maxTopGradient = 1.f;
inline constexpr uint32_t maxGeneratingTiles = 64;
// Chunks voxel tiles request cells for each update, highest priority first. Requesting for every waiting
// tile each update was a hash lookup per chunk for the whole ring, and only the first ones get generated.
inline constexpr int maxCellRequestsPerUpdate = 2048;

static int cellSizeLog2(int level)
{
    return std::max(0, std::countr_zero(chunkSizeXZ) + level - maxCellsPerSideLog2);
}

enum class LodTileState : uint8_t
{
    NEEDS_GEOMETRY,
    GENERATING_GEOMETRY,
    HAS_GEOMETRY,
};

class LodTile
{
public:
    // In tiles of this level
    const ivec2 tilePos;
    const int level;
    const bool isVoxel;

    // Main thread only
    LodTileState state{ LodTileState::NEEDS_GEOMETRY };
    // Unneeded while its geometry was generating; destroyed once that finishes
    bool isMarkedForDestruction{ false };
    bool isDisplayed{ false };
    bool needsGeometry{ false };
    bool subdivides{ false };
    bool childrenRenderable{ false };
    uint64_t lastNeededFrame{ 0 };
    uint64_t lastVisitedFrame{ 0 };

    Instance* terrainInstance{ nullptr };
    Instance* waterInstance{ nullptr };
    // A voxel tile's chunks and the ring around them, row by row, pinned while it meshes from them
    std::vector<const SurfaceChunkCells*> sourceCells;

    // Links to the tiles that exist, so visits don't look them up; each tile unlinks itself when destroyed
    LodTile* parent{ nullptr };
    std::array<LodTile*, 4> children{};

    LodTile(ivec2 tilePos, int level, bool isVoxel) : tilePos(tilePos), level(level), isVoxel(isVoxel) {}

    ~LodTile()
    {
        if (this->parent != nullptr)
        {
            this->parent->children[this->getChildIdx()] = nullptr;
        }
        for (LodTile* child : this->children)
        {
            if (child != nullptr)
            {
                child->parent = nullptr;
            }
        }
    }

    // Which of its parent's children it is
    int getChildIdx() const
    {
        return (this->tilePos.x & 1) + 2 * (this->tilePos.y & 1);
    }

    int getSideChunks() const
    {
        return 1 << this->level;
    }

    ivec2 getMinChunkPos() const
    {
        return this->tilePos * this->getSideChunks();
    }

    bool isReady() const
    {
        // Either instance is dropped when empty
        return this->state == LodTileState::HAS_GEOMETRY &&
               (this->terrainInstance == nullptr || this->terrainInstance->getHasBlas()) &&
               (this->waterInstance == nullptr || this->waterInstance->getHasBlas());
    }

    void setVisible(bool visible)
    {
        if (this->terrainInstance != nullptr)
        {
            this->terrainInstance->setVisible(visible);
        }
        if (this->waterInstance != nullptr)
        {
            this->waterInstance->setVisible(visible);
        }
    }

    void destroyInstances(ToFreeList& toFreeList)
    {
        if (this->terrainInstance != nullptr)
        {
            toFreeList.pushInstance(this->terrainInstance);
            this->terrainInstance = nullptr;
        }
        if (this->waterInstance != nullptr)
        {
            toFreeList.pushInstance(this->waterInstance);
            this->waterInstance = nullptr;
        }
        this->state = LodTileState::NEEDS_GEOMETRY;
    }

    void createGeometry(ThreadMemoryAllocator& threadMemoryAlloc);

private:
    // Each returns whether any face is a cutout, which needs the anyhit alpha test
    bool meshHeightfield(ThreadMemoryAllocator& threadMemoryAlloc);
    bool meshVoxels();
};

namespace TerrainLod
{

static Scene* scene;

static std::vector<LodTile*> tilesWithNewGeometry;
static std::mutex tilesWithNewGeometryMutex;

} // namespace TerrainLod

// Appends a vertex and returns its index
static uint32_t addVertex(HostGeometry& geometry, vec3 pos, vec3 normal, uint32_t tint)
{
    const DirectX::XMFLOAT3 normalDx{ normal.x, normal.y, normal.z };
    const PackedLodTerrainVertex packed = Util::packLodTerrainVertex({ pos.x, pos.y, pos.z }, normalDx, tint);
    geometry.packedTerrainVerts.push_back(std::bit_cast<PackedTerrainVertex>(packed));
    // Only the position feeds the BLAS, decoded so the traced and shaded surfaces agree
    geometry.verts.push_back({ Util::unpackLodTerrainPos(packed), Util::octEncode(normalDx), { 0.f, 0.f } });
    return static_cast<uint32_t>(geometry.verts.size() - 1);
}

// Two triangles over four corners in winding order, split along corners 0 and 2, as one face
static void addFace(HostGeometry& geometry, const std::array<uint32_t, 4>& corners, const PerFaceData& faceData)
{
    for (const uint32_t cornerIdx : { 0u, 1u, 2u, 0u, 2u, 3u })
    {
        geometry.idxs.push_back(corners[cornerIdx]);
    }
    geometry.perFaceDatas.push_back(faceData);
}

// One face of the box [boxMin, boxMax], each corner tinted by cornerTint(its local XZ)
template<typename CornerTint>
static void addBoxFace(HostGeometry& geometry,
                       BlockFace face,
                       vec3 boxMin,
                       vec3 boxMax,
                       const PerFaceData& faceData,
                       const CornerTint& cornerTint)
{
    const uint32_t faceIdx = blockFaceIndex(face);
    const ivec3* boxCorners = cubeFaceVertPositions + 4 * faceIdx;
    const vec3 normal(blockFaceBases[faceIdx].normal);
    std::array<uint32_t, 4> corners;
    for (uint32_t i = 0; i < 4; ++i)
    {
        const vec3 pos = boxMin + vec3(boxCorners[i]) * (boxMax - boxMin);
        corners[i] = addVertex(geometry, pos, normal, cornerTint(vec2(pos.x, pos.z)));
    }
    addFace(geometry, corners, faceData);
}

static PerFaceData blockFaceData(Block block, BlockFace face, uint32_t extraFlags = 0)
{
    const BlockData& blockData = Blocks::getBlockData(block);
    const bool isSide = blockFaceIndex(face) < blockFaceIndex(BlockFace::Y_POS);
    const BlockFace textureFace = isSide && blockData.lodSideShowsBottom ? BlockFace::Y_NEG : face;
    const uint32_t texSliceIdx = blockData.texSlices[std::max(static_cast<int>(blockFaceIndex(textureFace)) - 3, 0)];
    return TerrainMaterials::makeBlockFaceData(blockData, texSliceIdx,
                                               extraFlags | (isSide ? FACE_FLAG_SIDE_PROJECTION : 0u));
}

// Calls emit(startX, endX, z) for each run of cells in a row that canMerge(runStart, cell) accepts
template<typename CanMerge, typename Emit>
static void forEachRowRun(int numCells, const CanMerge& canMerge, const Emit& emit)
{
    for (int z = 0; z < numCells; ++z)
    {
        int runStartX = 0;
        for (int x = 1; x <= numCells; ++x)
        {
            if (x < numCells && canMerge(ivec2(runStartX, z), ivec2(x, z)))
            {
                continue;
            }
            emit(runStartX, x, z);
            runStartX = x;
        }
    }
}

bool LodTile::meshHeightfield(ThreadMemoryAllocator& threadMemoryAlloc)
{
    const int cellSize = 1 << cellSizeLog2(this->level);
    const int numCells = (static_cast<int>(chunkSizeXZ) << this->level) / cellSize;
    const ivec2 originXZ_WS = this->getMinChunkPos() * static_cast<int>(chunkSizeXZ);
    const float cellSizeF = static_cast<float>(cellSize);

    // Samples sit on cell corners. A margin of one before the first and two past the last gives every
    // corner, including the far edges', both neighbors for its normal, and the ice slabs their neighbors.
    const int numSamplesXZ = numCells + 3;
    ChunkGenerator::LodColumn* columns = threadMemoryAlloc.request<ChunkGenerator::LodColumn>(numSamplesXZ * numSamplesXZ);
    ChunkGenerator::LodRockStrata rockStrata;
    ChunkGenerator::sampleLodColumns(originXZ_WS - cellSize, cellSize, numSamplesXZ, columns, rockStrata,
                                     threadMemoryAlloc);
    const auto sampleIdxAt = [&](ivec2 cornerPos)
    {
        return static_cast<uint32_t>((cornerPos.x + 1) + numSamplesXZ * (cornerPos.y + 1));
    };
    const auto columnAt = [&](ivec2 cornerPos) -> const ChunkGenerator::LodColumn&
    {
        return columns[sampleIdxAt(cornerPos)];
    };
    // Half a block above where the density crosses zero, so the surface runs midway up the steps of the
    // chunks' block tops
    const auto heightAt = [&](ivec2 cornerPos)
    {
        return columnAt(cornerPos).surfaceHeight + 0.5f;
    };

    // Each corner takes the tint of the biome sampled there
    const auto cornerTint = [&](vec2 localXZ)
    {
        const glm::vec3& tint = Biomes::getBiomeData(columnAt(ivec2(round(localXZ / cellSizeF))).biome).grassTint;
        return Util::packUnorm8Rgb(tint.r, tint.g, tint.b);
    };

    HostGeometry& terrainGeometry = this->terrainInstance->hostGeometry;
    HostGeometry& waterGeometry = this->waterInstance->hostGeometry;

    // One shared vertex per corner for the gentle cells, with the normal of the surface through its neighbors
    const int numCornersXZ = numCells + 1;
    for (int z = 0; z < numCornersXZ; ++z)
    {
        for (int x = 0; x < numCornersXZ; ++x)
        {
            const ivec2 cornerPos(x, z);
            const vec3 normal = normalize(vec3(heightAt(cornerPos - ivec2(1, 0)) - heightAt(cornerPos + ivec2(1, 0)),
                                               2.f * cellSizeF,
                                               heightAt(cornerPos - ivec2(0, 1)) - heightAt(cornerPos + ivec2(0, 1))));
            addVertex(terrainGeometry, vec3(x * cellSizeF, heightAt(cornerPos), z * cellSizeF), normal,
                      cornerTint(vec2(cornerPos) * cellSizeF));
        }
    }
    const auto cornerVertIdx = [&](ivec2 cornerPos)
    {
        return static_cast<uint32_t>(cornerPos.x + numCornersXZ * cornerPos.y);
    };

    // A cell holds water, or the ice slab over it, wherever any of its corners is underwater, so the surface
    // reaches the shore and covers the part of the cell's slope below the waterline. Terrain above the water
    // level shows through it. Returns null for a dry cell.
    const auto cellWaterColumn = [&](ivec2 cellPos) -> const ChunkGenerator::LodColumn*
    {
        for (const ivec2 cornerOffset : { ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1) })
        {
            const ChunkGenerator::LodColumn& column = columnAt(cellPos + cornerOffset);
            if (column.topBlockY < column.waterLevel)
            {
                return &column;
            }
        }
        return nullptr;
    };
    // Ice slabs sit half a block below their block's top, as the terrain surface does: dry terrain never
    // dips below that, so no gap opens under a slab's edge on the shore side, where it has no side face
    constexpr float iceSlabHeight = 0.5f;
    const float waterTopHeight = blockShapeTopHeight(Blocks::getBlockData(Block::WATER_TOP).shape);
    // Where the water or ice over a cell's water column sits
    const auto waterSurfaceY = [&](const ChunkGenerator::LodColumn& waterColumn)
    {
        return static_cast<float>(waterColumn.waterLevel) +
               (waterColumn.waterTopBlock == Block::WATER_TOP ? waterTopHeight : iceSlabHeight);
    };

    const auto cellGradient = [&](float h00, float h10, float h01, float h11)
    {
        return vec2((h10 - h00) + (h11 - h01), (h01 - h00) + (h11 - h10)) / (2.f * cellSizeF);
    };
    const auto isSteepGradient = [&](vec2 gradient)
    {
        return dot(gradient, gradient) >= maxTopGradient * maxTopGradient;
    };

    // What a cell shows: its top block where the slope is gentle enough for block terrain to show mostly
    // tops, otherwise the side of its top block where the slope stays within the topsoil, else rock. A cell
    // holding water is judged by its highest corner and the slope above the water, which is all that shows:
    // judged down to the lakebed, every shore cell was rock, which outlined lakes and ice where it rose above
    // them.
    PerFaceData* cellFaceDatas = threadMemoryAlloc.request<PerFaceData>(numCells * numCells);
    for (int z = 0; z < numCells; ++z)
    {
        for (int x = 0; x < numCells; ++x)
        {
            const ivec2 cellPos(x, z);
            const float h00 = heightAt(cellPos);
            const float h10 = heightAt(cellPos + ivec2(1, 0));
            const float h01 = heightAt(cellPos + ivec2(0, 1));
            const float h11 = heightAt(cellPos + ivec2(1, 1));
            const vec2 gradient = cellGradient(h00, h10, h01, h11);
            const bool isSteep = isSteepGradient(gradient);

            const ChunkGenerator::LodColumn* const waterColumn = cellWaterColumn(cellPos);
            const float visibleFromY = waterColumn != nullptr ? waterSurfaceY(*waterColumn) : 0.f;
            const float v00 = std::max(h00, visibleFromY);
            const float v10 = std::max(h10, visibleFromY);
            const float v01 = std::max(h01, visibleFromY);
            const float v11 = std::max(h11, visibleFromY);
            const float minHeight = std::min({ v00, v10, v01, v11 });
            const float maxHeight = std::max({ v00, v10, v01, v11 });
            ivec2 shownCornerPos = cellPos;
            if (waterColumn != nullptr)
            {
                for (const ivec2 cornerOffset : { ivec2(1, 0), ivec2(0, 1), ivec2(1, 1) })
                {
                    if (heightAt(cellPos + cornerOffset) > heightAt(shownCornerPos))
                    {
                        shownCornerPos = cellPos + cornerOffset;
                    }
                }
            }
            const ChunkGenerator::LodColumn& column = columnAt(shownCornerPos);

            PerFaceData& faceData = cellFaceDatas[x + numCells * z];
            if (!isSteepGradient(cellGradient(v00, v10, v01, v11)))
            {
                faceData = blockFaceData(column.topBlock, BlockFace::Y_POS);
            }
            else if (maxHeight - minHeight <= static_cast<float>(column.soilDepth))
            {
                faceData = blockFaceData(column.topSideBlock, BlockFace::X_POS);
            }
            else
            {
                faceData = blockFaceData(rockStrata.atHeight(sampleIdxAt(shownCornerPos), 0.5f * (minHeight + maxHeight)),
                                         BlockFace::X_POS);
            }

            // In winding order, split along the first and third corners: the diagonal with less height change,
            // so ridges and valleys stay creased along their length rather than across it
            std::array<ivec2, 4> corners{ cellPos + ivec2(1, 1), cellPos + ivec2(1, 0), cellPos, cellPos + ivec2(0, 1) };
            if (std::abs(h11 - h00) > std::abs(h10 - h01))
            {
                std::rotate(corners.begin(), corners.begin() + 1, corners.end());
            }

            if (!isSteep)
            {
                addFace(terrainGeometry,
                        { cornerVertIdx(corners[0]), cornerVertIdx(corners[1]), cornerVertIdx(corners[2]),
                          cornerVertIdx(corners[3]) },
                        faceData);
                continue;
            }

            // Steep cells are faceted: on a cliff, a smooth normal averaged with the ground above and below
            // strays far from the long thin triangles' own, which streaks their shading and shadows
            const vec3 upward(-gradient.x, 1.f, -gradient.y);
            for (const std::array<int, 3>& triangle : { std::array<int, 3>{ 0, 1, 2 }, std::array<int, 3>{ 0, 2, 3 } })
            {
                std::array<vec3, 3> positions;
                for (int i = 0; i < 3; ++i)
                {
                    const ivec2 cornerPos = corners[triangle[i]];
                    positions[i] = vec3(cornerPos.x * cellSizeF, heightAt(cornerPos), cornerPos.y * cellSizeF);
                }
                vec3 normal = normalize(cross(positions[1] - positions[0], positions[2] - positions[0]));
                if (dot(normal, upward) < 0.f)
                {
                    normal = -normal;
                }
                for (int i = 0; i < 3; ++i)
                {
                    terrainGeometry.idxs.push_back(addVertex(terrainGeometry, positions[i], normal,
                                                             cornerTint(vec2(corners[triangle[i]]) * cellSizeF)));
                }
            }
            terrainGeometry.perFaceDatas.push_back(faceData);
        }
    }

    // Skirts hang below the tile's edges so a neighbor at another level never leaves a gap to see through
    const float skirtDepth = edgeSkirtDepthCells * cellSizeF;
    for (uint8_t faceIdx = 0; faceIdx < 4; ++faceIdx)
    {
        const ivec2 outward(blockFaceBases[faceIdx].normal.x, blockFaceBases[faceIdx].normal.z);
        const ivec2 along(abs(outward.y), abs(outward.x));
        // The edge's first corner: the far side for +x and +z, the near side for -x and -z
        const ivec2 edgeStart = max(outward, ivec2(0)) * numCells;
        for (int i = 0; i < numCells; ++i)
        {
            const ivec2 cornerA = edgeStart + along * i;
            const ivec2 cornerB = cornerA + along;
            const ivec2 cellPos = min(cornerA, ivec2(numCells - 1));
            const auto addSkirtVertex = [&](ivec2 cornerPos, float yOffset)
            {
                const float y = std::max(heightAt(cornerPos) + yOffset, 0.f);
                const vec3 pos(cornerPos.x * cellSizeF, y, cornerPos.y * cellSizeF);
                return addVertex(terrainGeometry, pos, vec3(outward.x, 0.f, outward.y),
                                 cornerTint(vec2(cornerPos) * cellSizeF));
            };
            addFace(terrainGeometry,
                    { addSkirtVertex(cornerA, 0.f), addSkirtVertex(cornerB, 0.f), addSkirtVertex(cornerB, -skirtDepth),
                      addSkirtVertex(cornerA, -skirtDepth) },
                    cellFaceDatas[cellPos.x + numCells * cellPos.y]);
        }
    }

    const PerFaceData waterFaceData =
        blockFaceData(Block::WATER_TOP, BlockFace::Y_POS, FACE_FLAG_IS_WATER | FACE_FLAG_IS_WATER_TOP);
    const auto isLiquid = [&](ivec2 cellPos)
    {
        const ChunkGenerator::LodColumn* column = cellWaterColumn(cellPos);
        return column != nullptr && column->waterTopBlock == Block::WATER_TOP;
    };
    forEachRowRun(numCells,
        [&](ivec2 runStartPos, ivec2 cellPos)
        {
            const ChunkGenerator::LodColumn* runStart = cellWaterColumn(runStartPos);
            const ChunkGenerator::LodColumn* column = cellWaterColumn(cellPos);
            return runStart != nullptr && column != nullptr && column->waterTopBlock == runStart->waterTopBlock &&
                   column->waterLevel == runStart->waterLevel;
        },
        [&](int startX, int endX, int z)
        {
            const ChunkGenerator::LodColumn* waterColumn = cellWaterColumn(ivec2(startX, z));
            if (waterColumn == nullptr)
            {
                return;
            }
            const ChunkGenerator::LodColumn& column = *waterColumn;
            const vec3 runMin(startX * cellSizeF, static_cast<float>(column.waterLevel), z * cellSizeF);
            const vec3 runMax(endX * cellSizeF, static_cast<float>(column.waterLevel), (z + 1) * cellSizeF);
            if (column.waterTopBlock == Block::WATER_TOP)
            {
                const vec3 waterOffset(0.f, waterTopHeight, 0.f);
                addBoxFace(waterGeometry, BlockFace::Y_POS, runMin + waterOffset, runMax + waterOffset, waterFaceData,
                        cornerTint);
            }
            else
            {
                const vec3 slabOffset(0.f, iceSlabHeight, 0.f);
                addBoxFace(terrainGeometry, BlockFace::Y_POS, runMin + slabOffset, runMax + slabOffset,
                        blockFaceData(column.waterTopBlock, BlockFace::Y_POS), cornerTint);
            }
        });

    // Ice slabs show their one-block edges over open water
    for (int z = 0; z < numCells; ++z)
    {
        for (int x = 0; x < numCells; ++x)
        {
            const ivec2 cellPos(x, z);
            const ChunkGenerator::LodColumn* waterColumn = cellWaterColumn(cellPos);
            if (waterColumn == nullptr || waterColumn->waterTopBlock == Block::WATER_TOP)
            {
                continue;
            }
            const float slabBottomY = static_cast<float>(waterColumn->waterLevel);
            for (uint8_t faceIdx = 0; faceIdx < 4; ++faceIdx)
            {
                const BlockFace face = static_cast<BlockFace>(faceIdx);
                const ivec2 neighborPos = cellPos + ivec2(blockFaceBases[faceIdx].normal.x, blockFaceBases[faceIdx].normal.z);
                if (!isLiquid(neighborPos))
                {
                    continue;
                }
                addBoxFace(terrainGeometry, face, vec3(x * cellSizeF, slabBottomY, z * cellSizeF),
                        vec3((x + 1) * cellSizeF, slabBottomY + iceSlabHeight, (z + 1) * cellSizeF),
                        blockFaceData(Block::ICE, face), cornerTint);
            }
        }
    }

    // Every top and side block is opaque
    return false;
}

bool LodTile::meshVoxels()
{
    const int sideChunks = this->getSideChunks();
    const int sourceSideChunks = sideChunks + 2;
    // By chunk offset from the tile's first chunk, including the ring around the tile
    const auto chunkCells = [&](ivec2 chunkOffset) -> const SurfaceChunkCells&
    {
        return *this->sourceCells[(chunkOffset.x + 1) + sourceSideChunks * (chunkOffset.y + 1)];
    };
    // Cell positions are relative to the tile in XZ and from the world's bottom in Y
    const auto cellAt = [&](ivec3 cellPos)
    {
        const ivec2 cellXZ(cellPos.x, cellPos.z);
        const ivec2 chunkOffset = glmUtil::floorDiv(cellXZ, ivec2(cellsPerChunkSide));
        return chunkCells(chunkOffset).cellAt(cellXZ - chunkOffset * cellsPerChunkSide, cellPos.y);
    };
    const int numCellsXZ = sideChunks * cellsPerChunkSide;

    // Solid cells compare fills as chunks compare shape heights; anything else sees a partial cell as a
    // layer, which hides nothing beside or below it
    const auto isCellFaceVisible = [&](const VoxelCell& cell, const VoxelCell& neighbor, uint8_t faceIdx)
    {
        const BlockData& blockData = Blocks::getBlockData(cell.block);
        const BlockData& neighborData = Blocks::getBlockData(neighbor.block);
        if (blockData.type != BlockType::SOLID || neighborData.type != BlockType::SOLID)
        {
            const BlockShape shape = cell.isFull() ? BlockShape::CUBE : BlockShape::LAYER;
            const BlockShape neighborShape = neighbor.isFull() ? BlockShape::CUBE : BlockShape::LAYER;
            return blockFaceVisible(blockData.type, shape, neighborData.type, neighborShape, faceIdx);
        }
        const BlockFace face = static_cast<BlockFace>(faceIdx);
        if (face == BlockFace::Y_POS)
        {
            return !cell.isFull();
        }
        if (face == BlockFace::Y_NEG)
        {
            return !neighbor.isFull();
        }
        return neighbor.fill < cell.fill;
    };

    // Cells cover at least the real blocks they stand for, so a neighboring chunk's cells may be solid
    // where its real blocks are air. Faces out of the tile are instead culled against those real blocks:
    // whatever is drawn there, chunks or another tile, covers at least them.
    const auto isFaceVisible = [&](ivec3 cellPos, const VoxelCell& cell, uint8_t faceIdx)
    {
        const ivec3 normal = blockFaceBases[faceIdx].normal;
        const ivec3 neighborCellPos = cellPos + normal;
        if (neighborCellPos.x >= 0 && neighborCellPos.x < numCellsXZ && neighborCellPos.z >= 0 &&
            neighborCellPos.z < numCellsXZ)
        {
            return isCellFaceVisible(cell, cellAt(neighborCellPos), faceIdx);
        }

        const ivec2 neighborCellXZ(neighborCellPos.x, neighborCellPos.z);
        const ivec2 neighborChunkOffset = glmUtil::floorDiv(neighborCellXZ, ivec2(cellsPerChunkSide));
        const ivec2 neighborLocalCellXZ = neighborCellXZ - neighborChunkOffset * cellsPerChunkSide;
        const SurfaceChunkCells& neighborChunk = chunkCells(neighborChunkOffset);
        // The neighbor's side facing this cell
        const int sideIdx = (faceIdx + 2) % 4;
        const int alongCell = normal.x != 0 ? neighborLocalCellXZ.y : neighborLocalCellXZ.x;

        // Each block across the edge faces the part of the cell's side within its own row
        const BlockData& blockData = Blocks::getBlockData(cell.block);
        for (int row = 0; row < voxelCellSize; ++row)
        {
            const int rowFillBottom = row * fillUnitsPerBlock;
            const int rowFillTop = std::min((row + 1) * fillUnitsPerBlock, static_cast<int>(cell.fill));
            if (rowFillTop <= rowFillBottom)
            {
                continue;
            }
            for (int along = 0; along < voxelCellSize; ++along)
            {
                const Block neighborBlock =
                    neighborChunk.sideBlockAt(sideIdx, alongCell * voxelCellSize + along, cellPos.y * voxelCellSize + row);
                const BlockData& neighborData = Blocks::getBlockData(neighborBlock);
                const bool isExposed = blockData.type == BlockType::SOLID && neighborData.type == BlockType::SOLID
                    ? rowFillBottom + blockFill(neighborData) < rowFillTop
                    : blockFaceVisible(blockData.type, BlockShape::CUBE, neighborData.type, neighborData.shape, faceIdx);
                if (isExposed)
                {
                    return true;
                }
            }
        }
        return false;
    };

    HostGeometry& terrainGeometry = this->terrainInstance->hostGeometry;
    HostGeometry& waterGeometry = this->waterInstance->hostGeometry;
    const PerFaceData waterFaceData =
        blockFaceData(Block::WATER_TOP, BlockFace::Y_POS, FACE_FLAG_IS_WATER | FACE_FLAG_IS_WATER_TOP);
    bool hasCutoutFaces = false;
    for (int chunkZ = 0; chunkZ < sideChunks; ++chunkZ)
    {
        for (int chunkX = 0; chunkX < sideChunks; ++chunkX)
        {
            const ivec2 chunkOffset(chunkX, chunkZ);
            const SurfaceChunkCells& cells = chunkCells(chunkOffset);
            // Below its band the chunk is solid, so its sides show down to the lowest neighboring band
            int minBlockY = cells.minBlockY;
            for (uint8_t sideIdx = 0; sideIdx < 4; ++sideIdx)
            {
                const ivec3 normal = blockFaceBases[sideIdx].normal;
                minBlockY = std::min(minBlockY, chunkCells(chunkOffset + ivec2(normal.x, normal.z)).minBlockY);
            }

            for (int cellY = minBlockY / voxelCellSize; cellY < cells.getMaxBlockY() / voxelCellSize; ++cellY)
            {
                for (int localZ = 0; localZ < cellsPerChunkSide; ++localZ)
                {
                    for (int localX = 0; localX < cellsPerChunkSide; ++localX)
                    {
                        const VoxelCell cell = cells.cellAt(ivec2(localX, localZ), cellY);
                        if (cell.block == Block::AIR)
                        {
                            continue;
                        }
                        const ivec3 cellPos(chunkX * cellsPerChunkSide + localX, cellY, chunkZ * cellsPerChunkSide + localZ);
                        const vec3 cellMin = vec3(cellPos) * static_cast<float>(voxelCellSize);
                        const vec3 cellMax = cellMin + vec3(voxelCellSize, static_cast<float>(cell.fill) / fillUnitsPerBlock,
                                                            voxelCellSize);
                        const uint32_t packedTint = cells.packedTints[localX + cellsPerChunkSide * localZ];
                        const auto cellTint = [&](vec2) { return packedTint; };

                        // Water shows only its surface, in the highest cell holding water: a water cell, or one
                        // holding both water and the ground under it
                        if (cell.waterFill > 0 && cellAt(cellPos + ivec3(0, 1, 0)).waterFill == 0)
                        {
                            const float waterTopY = cellMin.y + static_cast<float>(cell.waterFill) / fillUnitsPerBlock;
                            addBoxFace(waterGeometry, BlockFace::Y_POS, vec3(cellMin.x, waterTopY, cellMin.z),
                                       vec3(cellMax.x, waterTopY, cellMax.z), waterFaceData, cellTint);
                        }
                        if (Blocks::getBlockData(cell.block).type == BlockType::WATER)
                        {
                            continue;
                        }

                        for (uint8_t faceIdx = 0; faceIdx < blockFaceCount; ++faceIdx)
                        {
                            if (!isFaceVisible(cellPos, cell, faceIdx))
                            {
                                continue;
                            }
                            const BlockFace face = static_cast<BlockFace>(faceIdx);
                            const Block faceBlock = face == BlockFace::Y_POS ? cell.topBlock : cell.block;
                            addBoxFace(terrainGeometry, face, cellMin, cellMax, blockFaceData(faceBlock, face), cellTint);
                            hasCutoutFaces |= Blocks::getBlockData(faceBlock).type == BlockType::TRANSPARENT_CUTOUT;
                        }
                    }
                }
            }
        }
    }
    return hasCutoutFaces;
}

void LodTile::createGeometry(ThreadMemoryAllocator& threadMemoryAlloc)
{
    const bool hasCutoutFaces = this->isVoxel ? this->meshVoxels() : this->meshHeightfield(threadMemoryAlloc);
    HostGeometry& waterGeometry = this->waterInstance->hostGeometry;

    const ivec2 originXZ_WS = this->getMinChunkPos() * static_cast<int>(chunkSizeXZ);
    const ivec3 transformOffset(originXZ_WS.x, 0, originXZ_WS.y /*z*/);
    if (!this->terrainInstance->hostGeometry.verts.empty())
    {
        this->terrainInstance->setPackedVertexFormat(VERTEX_FORMAT_PACKED_LOD_TERRAIN);
        this->terrainInstance->setTransformOffset(transformOffset);
        this->terrainInstance->setTrisPerFaceLog2(1);
        this->terrainInstance->finalizeGeometry();
        this->terrainInstance->setMaterialIdx(TerrainMaterials::getMaterialIdx(TerrainMaterial::DEFAULT));
        // Tiles have no OMMs, so cutouts take the anyhit alpha test
        this->terrainInstance->setIsOpaque(!hasCutoutFaces);
    }

    if (!waterGeometry.verts.empty())
    {
        this->waterInstance->setPackedVertexFormat(VERTEX_FORMAT_PACKED_LOD_TERRAIN);
        this->waterInstance->setTransformOffset(transformOffset);
        this->waterInstance->setTrisPerFaceLog2(1);
        this->waterInstance->finalizeGeometry();
        this->waterInstance->setMaterialIdx(TerrainMaterials::getMaterialIdx(TerrainMaterial::WATER));
    }

    std::scoped_lock<std::mutex> lock(TerrainLod::tilesWithNewGeometryMutex);
    TerrainLod::tilesWithNewGeometry.push_back(this);
}

namespace TerrainLod
{

struct TileKey
{
    ivec2 tilePos;
    int level;

    bool operator==(const TileKey&) const = default;
};

struct TileKeyHash
{
    size_t operator()(const TileKey& key) const noexcept
    {
        return hash(key.tilePos.x ^ hash(key.tilePos.y ^ hash(key.level)));
    }
};

static std::unordered_map<TileKey, std::unique_ptr<LodTile>, TileKeyHash> tiles;
static uint32_t numGeneratingTiles{ 0 };
static uint64_t frame{ 0 };

// Rebuilt every update
static std::vector<std::pair<float, LodTile*>> generationCandidates;
static std::vector<LodTile*> displayedTiles;
static std::vector<ivec2> displayedChunkPositions;
// What the previous update showed, to hide whatever it no longer does
static std::vector<LodTile*> prevDisplayedTiles;
static std::vector<ivec2> prevDisplayedChunkPositions;
// Whether a tile stood in for children the camera wants this update
static bool hasStandIns{ false };
static bool isSettledNow{ false };

// Tiles at or below this level lie within a single region, which the walk finds once for all of them
inline constexpr int regionTileLevel = std::countr_zero(regionSideLength);

struct UpdateContext
{
    ivec2 cameraChunkPos;
    int chunkDistance;
    int voxelDistance;
    Region* (*findChunkRegion)(ivec2 chunkPos);

    // Chebyshev distances in chunks from the camera's chunk to the tile's nearest and farthest chunks
    int distanceTo(const LodTile& tile) const
    {
        const ivec2 minChunkPos = tile.getMinChunkPos();
        const ivec2 maxChunkPos = minChunkPos + tile.getSideChunks() - 1;
        const ivec2 axisDistance = max(max(minChunkPos - cameraChunkPos, cameraChunkPos - maxChunkPos), ivec2(0));
        return max(axisDistance.x, axisDistance.y);
    }

    int farthestDistanceTo(const LodTile& tile) const
    {
        const ivec2 minChunkPos = tile.getMinChunkPos();
        const ivec2 maxChunkPos = minChunkPos + tile.getSideChunks() - 1;
        const ivec2 axisDistance = max(abs(minChunkPos - cameraChunkPos), abs(maxChunkPos - cameraChunkPos));
        return max(axisDistance.x, axisDistance.y);
    }

    Chunk* findChunk(ivec2 chunkPos) const
    {
        Region* const region = this->findChunkRegion(chunkPos);
        return region == nullptr ? nullptr : region->getChunk(chunkPos);
    }

    // Null above regionTileLevel or where the region doesn't exist
    Region* findTileRegion(const LodTile& tile) const
    {
        return tile.level <= regionTileLevel ? this->findChunkRegion(tile.getMinChunkPos()) : nullptr;
    }

    Region* findChildRegion(const LodTile& child, Region* parentRegion) const
    {
        return child.level < regionTileLevel ? parentRegion : this->findTileRegion(child);
    }
};

// region is the level-0 tile's, from findTileRegion
static bool isChunkReady(const LodTile& tile, Region* region)
{
    return region != nullptr && region->isChunkGeometryReady(tile.tilePos);
}

void init(Scene* scene)
{
    TerrainLod::scene = scene;
}

static LodTile* findTile(ivec2 tilePos, int level)
{
    const auto tileIter = tiles.find({ tilePos, level });
    return tileIter == tiles.end() ? nullptr : tileIter->second.get();
}

static ivec2 childTilePos(const LodTile& tile, int childIdx)
{
    return tile.tilePos * 2 + ivec2(childIdx & 1, childIdx >> 1);
}

// A new tile links to its parent and children that already exist, as tiles outlive their relatives
static LodTile& getOrCreateTile(ivec2 tilePos, int level, const UpdateContext& ctx)
{
    std::unique_ptr<LodTile>& tile = tiles[{ tilePos, level }];
    if (tile == nullptr)
    {
        tile = std::make_unique<LodTile>(tilePos, level, ctx.voxelDistance > 0 && level <= maxVoxelTileLevel);
        LodTile* parent = findTile(glmUtil::floorDiv(tilePos, ivec2(2)), level + 1);
        if (parent != nullptr)
        {
            tile->parent = parent;
            parent->children[tile->getChildIdx()] = tile.get();
        }
        for (int childIdx = 0; level > 0 && childIdx < 4; ++childIdx)
        {
            LodTile* child = findTile(childTilePos(*tile, childIdx), level - 1);
            if (child != nullptr)
            {
                child->parent = tile.get();
                tile->children[childIdx] = child;
            }
        }
    }
    return *tile;
}

static LodTile& getOrCreateChild(LodTile& tile, int childIdx, const UpdateContext& ctx)
{
    if (tile.children[childIdx] == nullptr)
    {
        getOrCreateTile(childTilePos(tile, childIdx), tile.level - 1, ctx);
    }
    return *tile.children[childIdx];
}

static bool isCoveredByExisting(const LodTile& tile, Region* region, const UpdateContext& ctx);

// Calls func(chunkPos) for a voxel tile's chunks and the ring around them, row by row
template<typename Func>
static void forEachSourceChunk(const LodTile& tile, const Func& func)
{
    const ivec2 minChunkPos = tile.getMinChunkPos() - 1;
    const int sourceSideChunks = tile.getSideChunks() + 2;
    for (int z = 0; z < sourceSideChunks; ++z)
    {
        for (int x = 0; x < sourceSideChunks; ++x)
        {
            func(minChunkPos + ivec2(x, z));
        }
    }
}

// Requests every cell a voxel tile meshes from, so all of them progress, and returns whether all are ready
static bool requestSourceCells(const LodTile& tile, float priority)
{
    bool areAllReady = true;
    forEachSourceChunk(tile,
        [&](ivec2 chunkPos) { areAllReady &= SurfaceChunkCache::requestCells(chunkPos, priority) != nullptr; });
    return areAllReady;
}

// Whether the tile's chunk (level 0) or existing children are ready to cover it, whether or not they are
// still needed
static bool areChildrenCoveredByExisting(const LodTile& tile, Region* region, const UpdateContext& ctx)
{
    if (tile.level == 0)
    {
        return isChunkReady(tile, region);
    }
    for (int childIdx = 0; childIdx < 4; ++childIdx)
    {
        const LodTile* child = tile.children[childIdx];
        if (child == nullptr || !isCoveredByExisting(*child, ctx.findChildRegion(*child, region), ctx))
        {
            return false;
        }
    }
    return true;
}

static bool isCoveredByExisting(const LodTile& tile, Region* region, const UpdateContext& ctx)
{
    return tile.isReady() || areChildrenCoveredByExisting(tile, region, ctx);
}

// Visits the tiles the camera needs below this one, creating any that are missing, and returns whether
// the tile's area can be shown: by the tile itself or by its ready descendants and chunks. One tile
// that can't be shown makes every ancestor show itself instead, so coverage must not lapse. region is
// the tile's, from findTileRegion.
static bool visitNeededTile(LodTile& tile, Region* region, const UpdateContext& ctx)
{
    tile.lastNeededFrame = frame;
    tile.isMarkedForDestruction = false;

    const int distance = ctx.distanceTo(tile);
    tile.subdivides = distance <= ctx.chunkDistance ||
                      (tile.level > 0 && distance < (subdivideDistanceTiles << tile.level)) ||
                      (tile.level > maxVoxelTileLevel && distance <= ctx.voxelDistance);

    bool childrenRenderable = false;
    if (tile.subdivides)
    {
        if (tile.level == 0)
        {
            childrenRenderable = isChunkReady(tile, region);
        }
        else
        {
            childrenRenderable = true;
            for (int childIdx = 0; childIdx < 4; ++childIdx)
            {
                // Every child is visited, so none is left out of the needed tree
                LodTile& child = getOrCreateChild(tile, childIdx, ctx);
                childrenRenderable &= visitNeededTile(child, ctx.findChildRegion(child, region), ctx);
            }
        }
    }
    else
    {
        // A tile that stopped subdividing before it was regenerated is still covered by what it showed
        childrenRenderable = areChildrenCoveredByExisting(tile, region, ctx);
    }
    tile.childrenRenderable = childrenRenderable;

    const bool deepInChunkDistance =
        ctx.farthestDistanceTo(tile) <= ctx.chunkDistance - keepGeometryMarginChunks;
    tile.needsGeometry = !(deepInChunkDistance && (tile.level < minPlaceholderLevel || childrenRenderable));
    if (tile.needsGeometry && tile.state == LodTileState::NEEDS_GEOMETRY)
    {
        // Distance over level, as Distant Horizons orders it: coarse tiles go first while the area is
        // covered, but not so far ahead that tiles next to the chunks wait on the whole horizon, which
        // leaves the coarse ancestors standing in for them right by the camera
        const float priority = static_cast<float>(distance) / static_cast<float>(tile.level + 1);
        generationCandidates.emplace_back(priority, &tile);
    }

    return tile.isReady() || childrenRenderable;
}

// Only for tiles isCoveredByExisting accepts
static void displayExisting(LodTile& tile, const UpdateContext& ctx)
{
    tile.lastVisitedFrame = frame;
    if (tile.isReady())
    {
        displayedTiles.push_back(&tile);
    }
    else if (tile.level == 0)
    {
        displayedChunkPositions.push_back(tile.tilePos);
    }
    else
    {
        for (int childIdx = 0; childIdx < 4; ++childIdx)
        {
            displayExisting(*tile.children[childIdx], ctx);
        }
    }
}

// Shows the finest level the camera wants wherever all of it is ready, so each tile is swapped for its
// children, or the other way around, in a single frame. A tile not yet regenerated keeps showing the
// finer tiles or chunks that still cover it.
static void displayNeededTile(LodTile& tile, const UpdateContext& ctx)
{
    tile.lastVisitedFrame = frame;
    const bool showChildren = tile.childrenRenderable && (tile.subdivides || !tile.isReady());
    hasStandIns |= tile.subdivides && !tile.childrenRenderable;
    if (!showChildren)
    {
        if (tile.isReady())
        {
            displayedTiles.push_back(&tile);
        }
        return;
    }
    if (tile.level == 0)
    {
        displayedChunkPositions.push_back(tile.tilePos);
        return;
    }
    for (int childIdx = 0; childIdx < 4; ++childIdx)
    {
        LodTile& child = *tile.children[childIdx];
        if (tile.subdivides)
        {
            displayNeededTile(child, ctx);
        }
        else
        {
            displayExisting(child, ctx);
        }
    }
}

static bool isChunkPosLess(ivec2 a, ivec2 b)
{
    return a.x != b.x ? a.x < b.x : a.y < b.y;
}

// Shows what this update displays and hides what the last one did and this one doesn't, touching only
// what changed. The lists are kept sorted to diff them.
static void applyDisplayed(const UpdateContext& ctx)
{
    std::sort(displayedTiles.begin(), displayedTiles.end());
    std::sort(displayedChunkPositions.begin(), displayedChunkPositions.end(), isChunkPosLess);

    std::vector<LodTile*> changedTiles;
    std::set_difference(prevDisplayedTiles.begin(), prevDisplayedTiles.end(), displayedTiles.begin(),
                        displayedTiles.end(), std::back_inserter(changedTiles));
    for (LodTile* tile : changedTiles)
    {
        tile->isDisplayed = false;
        tile->setVisible(false);
    }
    changedTiles.clear();
    std::set_difference(displayedTiles.begin(), displayedTiles.end(), prevDisplayedTiles.begin(),
                        prevDisplayedTiles.end(), std::back_inserter(changedTiles));
    for (LodTile* tile : changedTiles)
    {
        tile->isDisplayed = true;
        tile->setVisible(true);
    }

    std::vector<ivec2> changedChunkPositions;
    std::set_difference(prevDisplayedChunkPositions.begin(), prevDisplayedChunkPositions.end(),
                        displayedChunkPositions.begin(), displayedChunkPositions.end(),
                        std::back_inserter(changedChunkPositions), isChunkPosLess);
    for (const ivec2 chunkPos : changedChunkPositions)
    {
        Chunk* chunk = ctx.findChunk(chunkPos);
        if (chunk != nullptr)
        {
            chunk->setInstancesVisible(false);
        }
    }
    changedChunkPositions.clear();
    std::set_difference(displayedChunkPositions.begin(), displayedChunkPositions.end(),
                        prevDisplayedChunkPositions.begin(), prevDisplayedChunkPositions.end(),
                        std::back_inserter(changedChunkPositions), isChunkPosLess);
    for (const ivec2 chunkPos : changedChunkPositions)
    {
        ctx.findChunk(chunkPos)->setInstancesVisible(true);
    }

    prevDisplayedTiles.swap(displayedTiles);
    displayedTiles.clear();
    prevDisplayedChunkPositions.swap(displayedChunkPositions);
    displayedChunkPositions.clear();
}

static void removeUnneededTiles(ToFreeList& toFreeList)
{
    for (auto tileIter = tiles.begin(); tileIter != tiles.end();)
    {
        LodTile& tile = *tileIter->second;
        if (tile.lastNeededFrame == frame || tile.lastVisitedFrame == frame)
        {
            if (!tile.needsGeometry && !tile.isDisplayed && tile.state == LodTileState::HAS_GEOMETRY)
            {
                tile.destroyInstances(toFreeList);
            }
            ++tileIter;
            continue;
        }

        if (tile.state == LodTileState::GENERATING_GEOMETRY)
        {
            tile.isMarkedForDestruction = true;
            ++tileIter;
            continue;
        }
        tile.destroyInstances(toFreeList);
        tileIter = tiles.erase(tileIter);
    }
}

static void task_createLodGeometry(const Task& task, ThreadMemoryAllocator& threadMemoryAlloc)
{
    task.lodTilePtr->createGeometry(threadMemoryAlloc);
}

static void startGeneratingTiles(ToFreeList& toFreeList, std::vector<Task>& outTasks)
{
    // Ties go to the coarser tile
    const auto isHigherPriority = [](const std::pair<float, LodTile*>& a, const std::pair<float, LodTile*>& b)
    {
        return a.first != b.first ? a.first < b.first : a.second->level > b.second->level;
    };
    std::sort(generationCandidates.begin(), generationCandidates.end(), isHigherPriority);

    // Voxel tiles request their cells whether or not they can start this update, so the cells for the
    // next ones are generated meanwhile
    int numCellRequestsLeft = maxCellRequestsPerUpdate;
    for (const auto& [priority, tilePtr] : generationCandidates)
    {
        LodTile& tile = *tilePtr;
        if (tile.isVoxel)
        {
            if (numCellRequestsLeft <= 0)
            {
                continue;
            }
            const int sourceSideChunks = tile.getSideChunks() + 2;
            numCellRequestsLeft -= sourceSideChunks * sourceSideChunks;
            if (!requestSourceCells(tile, priority))
            {
                continue;
            }
        }
        if (numGeneratingTiles >= maxGeneratingTiles)
        {
            if (numCellRequestsLeft <= 0)
            {
                break;
            }
            continue;
        }
        if (tile.isVoxel)
        {
            forEachSourceChunk(tile,
                [&](ivec2 chunkPos)
                {
                    tile.sourceCells.push_back(SurfaceChunkCache::requestCells(chunkPos, priority));
                    SurfaceChunkCache::pinCells(chunkPos);
                });
        }
        tile.state = LodTileState::GENERATING_GEOMETRY;
        ++numGeneratingTiles;

        tile.terrainInstance = scene->requestNewInstance(toFreeList, HostGeometrySize::LARGE);
        tile.waterInstance = scene->requestNewInstance(toFreeList, HostGeometrySize::SMALL);
        // Shown only once the tile is swapped in
        tile.setVisible(false);
        outTasks.push_back({ .func = task_createLodGeometry, .lodTilePtr = &tile });
    }
    generationCandidates.clear();
}

static void processTilesWithNewGeometry(ToFreeList& toFreeList)
{
    std::vector<LodTile*> tilesWithNewGeometryNow;
    {
        std::scoped_lock<std::mutex> lock(tilesWithNewGeometryMutex);
        tilesWithNewGeometryNow.swap(tilesWithNewGeometry);
    }

    for (LodTile* tile : tilesWithNewGeometryNow)
    {
        tile->state = LodTileState::HAS_GEOMETRY;
        --numGeneratingTiles;
        if (tile->isVoxel)
        {
            forEachSourceChunk(*tile, SurfaceChunkCache::unpinCells);
            tile->sourceCells.clear();
        }
        if (tile->isMarkedForDestruction)
        {
            tile->destroyInstances(toFreeList);
            tiles.erase({ tile->tilePos, tile->level });
            continue;
        }

        for (Instance** instance : { &tile->terrainInstance, &tile->waterInstance })
        {
            if ((*instance)->getIsGeometryFinalized())
            {
                scene->markInstanceReadyForBlasBuild(*instance);
            }
            else
            {
                toFreeList.pushInstance(*instance);
                *instance = nullptr;
            }
        }
    }
}

static int getRootLevel(int lodDistance)
{
    int level = 0;
    while ((subdivideDistanceTiles << level) < lodDistance)
    {
        ++level;
    }
    return level;
}

static void getRootTileBounds(ivec2 cameraChunkPos, int lodDistance, ivec2& outMinRootPos, ivec2& outMaxRootPos)
{
    const ivec2 rootSideChunks(1 << getRootLevel(lodDistance));
    outMinRootPos = glmUtil::floorDiv(cameraChunkPos - lodDistance, rootSideChunks);
    outMaxRootPos = glmUtil::floorDiv(cameraChunkPos + lodDistance, rootSideChunks);
}

void getCoveredChunkBounds(ivec2 cameraChunkPos, int lodDistance, ivec2& outMinChunkPos, ivec2& outMaxChunkPos)
{
    ivec2 minRootPos;
    ivec2 maxRootPos;
    getRootTileBounds(cameraChunkPos, lodDistance, minRootPos, maxRootPos);
    const int rootSideChunks = 1 << getRootLevel(lodDistance);
    outMinChunkPos = minRootPos * rootSideChunks;
    outMaxChunkPos = (maxRootPos + 1) * rootSideChunks - 1;
}

void update(ivec2 cameraChunkPos,
            int chunkDistance,
            int voxelDistance,
            int lodDistance,
            Region* (*findChunkRegion)(ivec2 chunkPos),
            ToFreeList& toFreeList,
            std::vector<Task>& outTasks)
{
    CPU_PROFILE_SCOPE("lod");
    ++frame;
    processTilesWithNewGeometry(toFreeList);

    const UpdateContext ctx{
        .cameraChunkPos = cameraChunkPos,
        .chunkDistance = chunkDistance,
        .voxelDistance = voxelDistance,
        .findChunkRegion = findChunkRegion,
    };

    const int rootLevel = getRootLevel(lodDistance);
    ivec2 minRootPos;
    ivec2 maxRootPos;
    getRootTileBounds(cameraChunkPos, lodDistance, minRootPos, maxRootPos);
    std::vector<LodTile*> roots;
    for (int rootZ = minRootPos.y; rootZ <= maxRootPos.y; ++rootZ)
    {
        for (int rootX = minRootPos.x; rootX <= maxRootPos.x; ++rootX)
        {
            LodTile& root = getOrCreateTile(ivec2(rootX, rootZ), rootLevel, ctx);
            visitNeededTile(root, ctx.findTileRegion(root), ctx);
            roots.push_back(&root);
        }
    }

    hasStandIns = false;
    for (LodTile* root : roots)
    {
        displayNeededTile(*root, ctx);
    }
    applyDisplayed(ctx);
    removeUnneededTiles(toFreeList);
    isSettledNow = !hasStandIns && generationCandidates.empty() && numGeneratingTiles == 0;
    startGeneratingTiles(toFreeList, outTasks);
    // Voxel tiles reach out to the voxel distance plus a tile, each reading the ring of chunks around it
    SurfaceChunkCache::update(cameraChunkPos, voxelDistance + (1 << maxVoxelTileLevel) + 1, outTasks);
}

bool isSettled()
{
    return isSettledNow;
}

void reset(ToFreeList& toFreeList)
{
    for (const auto& [key, tile] : tiles)
    {
        tile->destroyInstances(toFreeList);
    }
    tiles.clear();
    tilesWithNewGeometry.clear();
    generationCandidates.clear();
    displayedTiles.clear();
    displayedChunkPositions.clear();
    prevDisplayedTiles.clear();
    prevDisplayedChunkPositions.clear();
    numGeneratingTiles = 0;
    isSettledNow = false;
    SurfaceChunkCache::reset();
}

} // namespace TerrainLod
