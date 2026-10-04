// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "terrain_lod.h"

#include "biome.h"
#include "block.h"
#include "block_orientation.h"
#include "chunk.h"
#include "chunk_generator.h"
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
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace glm;

// A tile at level L covers 2^L x 2^L chunks with at most 2^maxCellsPerSideLog2 cells per side, so cells
// are single blocks up to the level where that many cells span the tile and double in size every
// level above it
inline constexpr int maxCellsPerSideLog2 = 7;
// A tile is replaced by its children within this many of its own widths of the camera, so a cell spans
// about the same angle wherever its level is shown
inline constexpr int subdivideDistanceTiles = 2;
// Tiles up to this level are voxel tiles where voxel tiles are on: surface-only chunks with their
// structures, downsampled. Tiles within the voxel distance are subdivided down to this level, so
// structures and 3D landforms continue past the chunk distance.
inline constexpr int maxVoxelTileLevel = 2;
// Voxel cells are this many blocks on a side
inline constexpr int voxelCellSize = 2;
// Generating a voxel tile means generating all its chunks and their margin, so few run at once; each
// holds that many chunks' block buffers, which the chunk buffer pool keeps afterward
inline constexpr uint32_t maxGeneratingVoxelTiles = 4;
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
inline constexpr uint32_t maxGeneratingTiles = 16;

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

    LodTile(ivec2 tilePos, int level, bool isVoxel) : tilePos(tilePos), level(level), isVoxel(isVoxel) {}

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
    bool meshVoxels(ThreadMemoryAllocator& threadMemoryAlloc);
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
    const int faceIdx = static_cast<int>(blockFaceIndex(face));
    const uint32_t texSliceIdx = blockData.texSlices[std::max(faceIdx - 3, 0)];
    const bool isSide = faceIdx < blockFaceIndex(BlockFace::Y_POS);
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
        return Util::packUnorm8(tint.r) | (Util::packUnorm8(tint.g) << 8) | (Util::packUnorm8(tint.b) << 16);
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

    // What a cell shows: its top block where the slope is gentle enough for block terrain to show mostly
    // tops, otherwise the side of its top block where the slope stays within the topsoil, else rock
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
            const vec2 gradient = vec2((h10 - h00) + (h11 - h01), (h01 - h00) + (h11 - h10)) / (2.f * cellSizeF);
            const float minHeight = std::min({ h00, h10, h01, h11 });
            const float maxHeight = std::max({ h00, h10, h01, h11 });
            const ChunkGenerator::LodColumn& column = columnAt(cellPos);

            PerFaceData& faceData = cellFaceDatas[x + numCells * z];
            const bool isSteep = dot(gradient, gradient) >= maxTopGradient * maxTopGradient;
            if (!isSteep)
            {
                faceData = blockFaceData(column.topBlock, BlockFace::Y_POS);
            }
            else if (maxHeight - minHeight <= static_cast<float>(column.soilDepth))
            {
                faceData = blockFaceData(column.topSideBlock, BlockFace::X_POS);
            }
            else
            {
                faceData = blockFaceData(rockStrata.atHeight(sampleIdxAt(cellPos), 0.5f * (minHeight + maxHeight)),
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

    const float waterTopHeight = blockShapeTopHeight(Blocks::getBlockData(Block::WATER_TOP).shape);
    const PerFaceData waterFaceData =
        blockFaceData(Block::WATER_TOP, BlockFace::Y_POS, FACE_FLAG_IS_WATER | FACE_FLAG_IS_WATER_TOP);
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
                const vec3 slabOffset(0.f, 1.f, 0.f);
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
                        vec3((x + 1) * cellSizeF, slabBottomY + 1.f, (z + 1) * cellSizeF),
                        blockFaceData(Block::ICE, face), cornerTint);
            }
        }
    }

    // Every top and side block is opaque
    return false;
}

// Heights in a cell are in eighths of a block, the granularity of block shape heights
inline constexpr int fillUnitsPerBlock = 8;
inline constexpr int fullCellFill = voxelCellSize * fillUnitsPerBlock;

// A downsampled cell: filling the cell from its bottom up to fill, with its block's sides and the top of
// the block on top, as a snow layer on grass shows snow above the grass's sides
struct VoxelCell
{
    Block block{ Block::AIR };
    Block topBlock{ Block::AIR };
    uint8_t fill{ 0 };

    bool isFull() const
    {
        return this->fill == fullCellFill;
    }
};

// Blocks that fill their cell from the bottom up to their shape's top height, which are the ones cells
// can show. Plants and models are too small to.
static bool fillsFromBottom(const BlockData& blockData)
{
    return blockData.type != BlockType::AIR && blockData.type != BlockType::WATER && !isDecoratorShape(blockData.shape);
}

static int blockFill(const BlockData& blockData)
{
    return static_cast<int>(blockShapeTopHeight(blockData.shape) * fillUnitsPerBlock);
}

// Of a 2x2x2 group of blocks, bottom four first, the cell's block is the most common that fills from the
// bottom, preferring whole blocks over partial ones lying on them and going to the higher on ties, so
// canopies and thin pillars survive and surfaces keep their top blocks; else water if any; else air. The
// cell fills up to its highest column, counting a block on top as standing on a full one, so it covers at
// least the blocks it stands for and keeps their shapes' heights (a snow layer on the ground tops the
// cell an eighth of a block above it), and that column's top block shows on top.
static VoxelCell downsampleBlocks(const std::array<Block, 8>& blocks)
{
    VoxelCell cell;
    int bestCount = 0;
    bool bestIsWhole = false;
    bool hasWater = false;
    for (int i = 0; i < 8; ++i)
    {
        const BlockData& blockData = Blocks::getBlockData(blocks[i]);
        if (blockData.type == BlockType::WATER)
        {
            hasWater = true;
            continue;
        }
        if (!fillsFromBottom(blockData))
        {
            continue;
        }
        const bool isWhole = blockFill(blockData) == fillUnitsPerBlock;
        const int count = static_cast<int>(std::count(blocks.begin(), blocks.end(), blocks[i]));
        if (isWhole > bestIsWhole || (isWhole == bestIsWhole && count >= bestCount))
        {
            cell.block = blocks[i];
            bestCount = count;
            bestIsWhole = isWhole;
        }
    }
    if (cell.block == Block::AIR)
    {
        if (hasWater)
        {
            cell = { Block::WATER, Block::WATER, fullCellFill };
        }
        return cell;
    }

    int fill = 0;
    for (int column = 0; column < 4; ++column)
    {
        for (const int i : { column + 4, column })
        {
            const BlockData& blockData = Blocks::getBlockData(blocks[i]);
            if (!fillsFromBottom(blockData))
            {
                continue;
            }
            const int columnFill = (i >= 4 ? fillUnitsPerBlock : 0) + blockFill(blockData);
            if (columnFill > fill)
            {
                fill = columnFill;
                cell.topBlock = blocks[i];
            }
            break;
        }
    }
    cell.fill = static_cast<uint8_t>(fill);
    return cell;
}

bool LodTile::meshVoxels(ThreadMemoryAllocator& threadMemoryAlloc)
{
    const ivec2 minChunkPos = this->getMinChunkPos();
    const SurfaceOnlyChunks area = Chunk::generateSurfaceOnly(minChunkPos, this->getSideChunks(), threadMemoryAlloc);
    const ivec2 originXZ_WS = minChunkPos * static_cast<int>(chunkSizeXZ);
    const auto chunkAndLocalPos = [&](ivec2 posXZ_WS, ivec2& outPosXZ_CS) -> const Chunk&
    {
        const ivec2 chunkPos = glmUtil::floorDiv(posXZ_WS, ivec2(chunkSizeXZ));
        outPosXZ_CS = posXZ_WS - chunkPos * static_cast<int>(chunkSizeXZ);
        return area.chunkAt(chunkPos);
    };
    const auto blockAt = [&](ivec3 pos_WS)
    {
        ivec2 posXZ_CS;
        const Chunk& chunk = chunkAndLocalPos(ivec2(pos_WS.x, pos_WS.z), posXZ_CS);
        return chunk.getGeneratedBlock(uvec3(posXZ_CS.x, pos_WS.y, posXZ_CS.y));
    };

    // Cells cover the tile plus one cell of margin from the surrounding chunks, which only culls faces
    const int numCellsXZ = this->getSideChunks() * static_cast<int>(chunkSizeXZ) / voxelCellSize;
    const int numGridCellsXZ = numCellsXZ + 2;
    const auto cellToBlockXZ = [&](ivec2 cellXZ)
    {
        return originXZ_WS + cellXZ * voxelCellSize;
    };

    // The cells span the band from below the lowest solid column top (under any water) to the highest top;
    // below it is all solid
    int lowestTopY = static_cast<int>(chunkSizeY);
    int highestTopY = 0;
    for (int z = -1; z <= numCellsXZ; ++z)
    {
        for (int x = -1; x <= numCellsXZ; ++x)
        {
            for (const ivec2 blockOffset : { ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1) })
            {
                const ivec2 posXZ_WS = cellToBlockXZ(ivec2(x, z)) + blockOffset;
                int topY = static_cast<int>(chunkSizeY) - 1;
                while (topY > 0 && blockAt(ivec3(posXZ_WS.x, topY, posXZ_WS.y)) == Block::AIR)
                {
                    --topY;
                }
                highestTopY = std::max(highestTopY, topY);
                while (topY > 0 && Blocks::getBlockData(blockAt(ivec3(posXZ_WS.x, topY, posXZ_WS.y))).type == BlockType::WATER)
                {
                    --topY;
                }
                lowestTopY = std::min(lowestTopY, topY);
            }
        }
    }
    const int bandMinY = std::max(lowestTopY - 2 * voxelCellSize, 0) / voxelCellSize * voxelCellSize;
    const int numCellsY = (highestTopY - bandMinY) / voxelCellSize + 1;

    VoxelCell* cells = threadMemoryAlloc.request<VoxelCell>(numGridCellsXZ * numGridCellsXZ * numCellsY);
    const auto cellIdx = [&](ivec3 cellPos)
    {
        return (cellPos.x + 1) + numGridCellsXZ * ((cellPos.z + 1) + numGridCellsXZ * cellPos.y);
    };
    for (int y = 0; y < numCellsY; ++y)
    {
        for (int z = -1; z <= numCellsXZ; ++z)
        {
            for (int x = -1; x <= numCellsXZ; ++x)
            {
                const ivec2 blockXZ = cellToBlockXZ(ivec2(x, z));
                const int blockY = bandMinY + y * voxelCellSize;
                std::array<Block, 8> blocks;
                for (int i = 0; i < 8; ++i)
                {
                    const int sourceY = blockY + (i >> 2);
                    blocks[i] = sourceY < static_cast<int>(chunkSizeY)
                        ? blockAt(ivec3(blockXZ.x + (i & 1), sourceY, blockXZ.y + ((i >> 1) & 1)))
                        : Block::AIR;
                }
                cells[cellIdx(ivec3(x, y, z))] = downsampleBlocks(blocks);
            }
        }
    }
    const auto cellAt = [&](ivec3 cellPos) -> VoxelCell
    {
        if (cellPos.y < 0)
        {
            return { Block::STONE, Block::STONE, fullCellFill };
        }
        if (cellPos.y >= numCellsY)
        {
            return {};
        }
        return cells[cellIdx(cellPos)];
    };
    const auto cellBlockPos = [&](ivec3 cellPos)
    {
        const ivec2 blockXZ = cellToBlockXZ(ivec2(cellPos.x, cellPos.z));
        return ivec3(blockXZ.x, bandMinY + cellPos.y * voxelCellSize, blockXZ.y);
    };
    const auto blockAtOrAir = [&](ivec3 pos_WS)
    {
        return pos_WS.y < static_cast<int>(chunkSizeY) ? blockAt(pos_WS) : Block::AIR;
    };

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

    // Margin cells cover at least the real blocks beside the tile, so they may be solid where those are air.
    // Faces out of the tile are instead culled against the real blocks: whatever is drawn there, chunks or
    // another tile, covers at least them.
    const auto isFaceVisible = [&](ivec3 cellPos, const VoxelCell& cell, uint8_t faceIdx)
    {
        const ivec3 normal = blockFaceBases[faceIdx].normal;
        const ivec3 neighborCellPos = cellPos + normal;
        if (neighborCellPos.x >= 0 && neighborCellPos.x < numCellsXZ && neighborCellPos.z >= 0 &&
            neighborCellPos.z < numCellsXZ)
        {
            return isCellFaceVisible(cell, cellAt(neighborCellPos), faceIdx);
        }

        // Each block across the edge faces the part of the cell's side within its own row
        const BlockData& blockData = Blocks::getBlockData(cell.block);
        for (int i = 0; i < 8; ++i)
        {
            const int row = i >> 2;
            const int rowFillTop = std::min((row + 1) * fillUnitsPerBlock, static_cast<int>(cell.fill));
            const int rowFillBottom = row * fillUnitsPerBlock;
            const ivec3 acrossOffset = ivec3(i & 1, row, (i >> 1) & 1) + normal;
            if (rowFillTop <= rowFillBottom ||
                (glm::all(glm::greaterThanEqual(acrossOffset, ivec3(0))) &&
                 glm::all(glm::lessThan(acrossOffset, ivec3(voxelCellSize)))))
            {
                continue;
            }
            const BlockData& neighborData = Blocks::getBlockData(blockAtOrAir(cellBlockPos(cellPos) + acrossOffset));
            const bool isExposed = blockData.type == BlockType::SOLID && neighborData.type == BlockType::SOLID
                ? rowFillBottom + blockFill(neighborData) < rowFillTop
                : blockFaceVisible(blockData.type, BlockShape::CUBE, neighborData.type, neighborData.shape, faceIdx);
            if (isExposed)
            {
                return true;
            }
        }
        return false;
    };

    // The height of the cell's highest water, if it holds any
    const auto cellWaterTopY = [&](ivec3 cellPos) -> std::optional<float>
    {
        std::optional<float> topY;
        for (int i = 0; i < 8; ++i)
        {
            const ivec3 pos_WS = cellBlockPos(cellPos) + ivec3(i & 1, i >> 2, (i >> 1) & 1);
            const BlockData& sourceData = Blocks::getBlockData(blockAtOrAir(pos_WS));
            if (sourceData.type == BlockType::WATER)
            {
                topY = std::max(topY.value_or(0.f), pos_WS.y + blockShapeTopHeight(sourceData.shape));
            }
        }
        return topY;
    };
    // Water shows only its surface, in the highest cell holding water: a water cell, or one holding both
    // water and the ground under it
    const auto waterSurfaceY = [&](ivec3 cellPos) -> std::optional<float>
    {
        const ivec3 abovePos = cellPos + ivec3(0, 1, 0);
        if (abovePos.y < numCellsY && cellWaterTopY(abovePos).has_value())
        {
            return std::nullopt;
        }
        return cellWaterTopY(cellPos);
    };

    HostGeometry& terrainGeometry = this->terrainInstance->hostGeometry;
    HostGeometry& waterGeometry = this->waterInstance->hostGeometry;
    const PerFaceData waterFaceData =
        blockFaceData(Block::WATER_TOP, BlockFace::Y_POS, FACE_FLAG_IS_WATER | FACE_FLAG_IS_WATER_TOP);
    bool hasCutoutFaces = false;
    for (int y = 0; y < numCellsY; ++y)
    {
        for (int z = 0; z < numCellsXZ; ++z)
        {
            for (int x = 0; x < numCellsXZ; ++x)
            {
                const ivec3 cellPos(x, y, z);
                const VoxelCell cell = cellAt(cellPos);
                if (cell.block == Block::AIR)
                {
                    continue;
                }
                const BlockData& blockData = Blocks::getBlockData(cell.block);
                const vec3 cellMin(x * voxelCellSize, bandMinY + y * voxelCellSize, z * voxelCellSize);
                const vec3 cellMax = cellMin + vec3(voxelCellSize, static_cast<float>(cell.fill) / fillUnitsPerBlock,
                                                    voxelCellSize);

                ivec2 posXZ_CS;
                const Chunk& chunk = chunkAndLocalPos(cellToBlockXZ(ivec2(x, z)), posXZ_CS);
                const glm::vec3& tint = Biomes::getBiomeData(chunk.getBiomes()[posXZ_CS.x + chunkSizeXZ * posXZ_CS.y]).grassTint;
                const uint32_t packedTint =
                    Util::packUnorm8(tint.r) | (Util::packUnorm8(tint.g) << 8) | (Util::packUnorm8(tint.b) << 16);
                const auto cellTint = [&](vec2) { return packedTint; };

                if (blockData.type == BlockType::WATER || !cell.isFull())
                {
                    const std::optional<float> topY = waterSurfaceY(cellPos);
                    if (topY.has_value())
                    {
                        addBoxFace(waterGeometry, BlockFace::Y_POS, vec3(cellMin.x, *topY, cellMin.z),
                                   vec3(cellMax.x, *topY, cellMax.z), waterFaceData, cellTint);
                    }
                }
                if (blockData.type == BlockType::WATER)
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
    return hasCutoutFaces;
}

void LodTile::createGeometry(ThreadMemoryAllocator& threadMemoryAlloc)
{
    const bool hasCutoutFaces = this->isVoxel ? this->meshVoxels(threadMemoryAlloc) : this->meshHeightfield(threadMemoryAlloc);
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

struct IVec2Hash
{
    size_t operator()(const ivec2& v) const noexcept
    {
        return hash(v.x ^ hash(v.y));
    }
};

static std::unordered_map<TileKey, std::unique_ptr<LodTile>, TileKeyHash> tiles;
static uint32_t numGeneratingTiles{ 0 };
static uint32_t numGeneratingVoxelTiles{ 0 };
static uint64_t frame{ 0 };

// Rebuilt every update
static std::vector<std::pair<float, LodTile*>> generationCandidates;
static std::vector<LodTile*> displayedTiles;
static std::vector<ivec2> displayedChunkPositions;
// What the previous update showed, to hide whatever it no longer does
static std::vector<LodTile*> prevDisplayedTiles;
static std::vector<ivec2> prevDisplayedChunkPositions;
static std::unordered_set<ivec2, IVec2Hash> displayedChunkSet;

struct UpdateContext
{
    ivec2 cameraChunkPos;
    int chunkDistance;
    int voxelDistance;
    Chunk* (*findChunk)(ivec2 chunkPos);

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

    bool isChunkReady(ivec2 chunkPos) const
    {
        const Chunk* chunk = this->findChunk(chunkPos);
        if (chunk == nullptr || chunk->getState() != ChunkState::HAS_GEOMETRY || chunk->getIsMarkedForDestruction())
        {
            return false;
        }
        const Instance* waterInstance = chunk->getWaterInstance();
        return chunk->getTerrainInstance()->getHasBlas() && (waterInstance == nullptr || waterInstance->getHasBlas());
    }
};

void init(Scene* scene)
{
    TerrainLod::scene = scene;
}

static LodTile* findTile(ivec2 tilePos, int level)
{
    const auto tileIter = tiles.find({ tilePos, level });
    return tileIter == tiles.end() ? nullptr : tileIter->second.get();
}

static LodTile& getOrCreateTile(ivec2 tilePos, int level, const UpdateContext& ctx)
{
    std::unique_ptr<LodTile>& tile = tiles[{ tilePos, level }];
    if (tile == nullptr)
    {
        tile = std::make_unique<LodTile>(tilePos, level, ctx.voxelDistance > 0 && level <= maxVoxelTileLevel);
    }
    return *tile;
}

static ivec2 childTilePos(const LodTile& tile, int childIdx)
{
    return tile.tilePos * 2 + ivec2(childIdx & 1, childIdx >> 1);
}

static bool isCoveredByExisting(const LodTile& tile, const UpdateContext& ctx);

// Whether the tile's chunk (level 0) or existing children are ready to cover it, whether or not they are
// still needed
static bool areChildrenCoveredByExisting(const LodTile& tile, const UpdateContext& ctx)
{
    if (tile.level == 0)
    {
        return ctx.isChunkReady(tile.tilePos);
    }
    for (int childIdx = 0; childIdx < 4; ++childIdx)
    {
        const LodTile* child = findTile(childTilePos(tile, childIdx), tile.level - 1);
        if (child == nullptr || !isCoveredByExisting(*child, ctx))
        {
            return false;
        }
    }
    return true;
}

static bool isCoveredByExisting(const LodTile& tile, const UpdateContext& ctx)
{
    return tile.isReady() || areChildrenCoveredByExisting(tile, ctx);
}

// Visits the tiles the camera needs below this one, creating any that are missing, and returns whether
// the tile's area can be shown: by the tile itself or by its ready descendants and chunks. One tile
// that can't be shown makes every ancestor show itself instead, so coverage must not lapse.
static bool visitNeededTile(LodTile& tile, const UpdateContext& ctx)
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
            childrenRenderable = ctx.isChunkReady(tile.tilePos);
        }
        else
        {
            childrenRenderable = true;
            for (int childIdx = 0; childIdx < 4; ++childIdx)
            {
                // Every child is visited, so none is left out of the needed tree
                childrenRenderable &= visitNeededTile(getOrCreateTile(childTilePos(tile, childIdx), tile.level - 1, ctx), ctx);
            }
        }
    }
    else
    {
        // A tile that stopped subdividing before it was regenerated is still covered by what it showed
        childrenRenderable = areChildrenCoveredByExisting(tile, ctx);
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
            displayExisting(*findTile(childTilePos(tile, childIdx), tile.level - 1), ctx);
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
        LodTile& child = *findTile(childTilePos(tile, childIdx), tile.level - 1);
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

static void applyDisplayed(const UpdateContext& ctx)
{
    for (LodTile* tile : prevDisplayedTiles)
    {
        tile->isDisplayed = false;
    }
    for (LodTile* tile : displayedTiles)
    {
        tile->isDisplayed = true;
    }
    for (LodTile* tile : prevDisplayedTiles)
    {
        tile->setVisible(tile->isDisplayed);
    }
    for (LodTile* tile : displayedTiles)
    {
        tile->setVisible(true);
    }

    displayedChunkSet.clear();
    displayedChunkSet.insert(displayedChunkPositions.begin(), displayedChunkPositions.end());
    for (const ivec2 chunkPos : prevDisplayedChunkPositions)
    {
        Chunk* chunk = ctx.findChunk(chunkPos);
        if (chunk != nullptr && !displayedChunkSet.contains(chunkPos))
        {
            chunk->setInstancesVisible(false);
        }
    }
    for (const ivec2 chunkPos : displayedChunkPositions)
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

    for (const auto& [priority, tilePtr] : generationCandidates)
    {
        if (numGeneratingTiles >= maxGeneratingTiles)
        {
            break;
        }
        LodTile& tile = *tilePtr;
        if (tile.isVoxel)
        {
            if (numGeneratingVoxelTiles >= maxGeneratingVoxelTiles)
            {
                continue;
            }
            ++numGeneratingVoxelTiles;
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
            --numGeneratingVoxelTiles;
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

bool isChunkDisplayed(ivec2 chunkPos)
{
    return displayedChunkSet.contains(chunkPos);
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
            Chunk* (*findChunk)(ivec2 chunkPos),
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
        .findChunk = findChunk,
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
            visitNeededTile(root, ctx);
            roots.push_back(&root);
        }
    }

    for (LodTile* root : roots)
    {
        displayNeededTile(*root, ctx);
    }
    applyDisplayed(ctx);
    removeUnneededTiles(toFreeList);
    startGeneratingTiles(toFreeList, outTasks);
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
    displayedChunkSet.clear();
    numGeneratingTiles = 0;
    numGeneratingVoxelTiles = 0;
}

} // namespace TerrainLod
