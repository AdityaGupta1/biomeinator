// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include "biome.h"
#include "block.h"
#include "cave_biome.h"
#include "cave_biome_noise.h"
#include "chunk_dimensions.h"
#include "terrain.h"
#include "scene/scene.h"
#include "serialized_chunk.h"
#include "structure/cave_structure.h"
#include "structure/structure.h"
#include "util/math.h"

#include <array>
#include <atomic>
#include <bitset>
#include <glm/glm.hpp>
#include <unordered_map>

enum class ChunkState : uint8_t
{
    NEEDS_TERRAIN,
    GENERATING_TERRAIN,
    HAS_TERRAIN,
    AWAITING_STRUCTURE_NEIGHBORS,
    NEEDS_FILL_STRUCTURES, // chunks in structureMaxChunkRadius all have structures (>= HAS_TERRAIN)
    FILLING_STRUCTURES,
    HAS_ALL_BLOCKS,
    NEEDS_SEGMENTS, // neighbor chunks all have blocks (>= HAS_ALL_BLOCKS)
    GENERATING_SEGMENTS,
    NEEDS_GEOMETRY,
    GENERATING_GEOMETRY,
    HAS_GEOMETRY,
};

enum class NeighborDirection : uint8_t
{
    X_POS = 0,
    Z_POS = 1,
    X_NEG = 2,
    Z_NEG = 3,
};

constexpr glm::ivec2 neighborOffset(NeighborDirection dir)
{
    switch (dir)
    {
        case NeighborDirection::X_POS:
            return { 1, 0 };
        case NeighborDirection::Z_POS:
            return { 0, 1};
        case NeighborDirection::X_NEG:
            return { -1, 0 };
        case NeighborDirection::Z_NEG:
            return { 0, -1 };
    }

    return { 0, 0 };
}

constexpr NeighborDirection oppositeNeighborDirection(NeighborDirection dir)
{
    return static_cast<NeighborDirection>((static_cast<uint8_t>(dir) + 2) & 0x3);
}

enum class ChunkSegment : uint8_t
{
    AIR,
    SOLID_SURROUNDED,
    MIXED,
};

inline constexpr glm::ivec3 chunkSizeVec = { chunkSizeXZ, chunkSizeY, chunkSizeXZ };
inline constexpr uint32_t caveMaxY = 320;

static_assert(MathUtil::isPowerOfTwo(chunkSizeXZ), "chunkSizeXZ must be a power of two");
static_assert(caveMaxY <= chunkSizeY);

inline constexpr uint32_t chunkSegmentSizeXZ = 4;
inline constexpr uint32_t chunkSegmentSizeY = 8;

static_assert(chunkSizeXZ % chunkSegmentSizeXZ == 0, "chunkSizeXZ must be a multiple of chunkSegmentSizeXZ");
static_assert(chunkSizeY % chunkSegmentSizeY == 0, "chunkSizeY must be a multiple of chunkSegmentSizeY");

inline constexpr uint32_t numChunkSegmentsXZ = chunkSizeXZ / chunkSegmentSizeXZ;
inline constexpr uint32_t numChunkSegmentsY = chunkSizeY / chunkSegmentSizeY;
inline constexpr uint32_t numChunkSegments = numChunkSegmentsXZ * numChunkSegmentsY * numChunkSegmentsXZ;

class Region;
class ThreadMemoryAllocator;

// Chunk-owned inputs for deferred cave decoration. Neighboring chunks read only
// the immutable terrain masks; these fields live until this chunk finishes decoration.
struct CaveDecorationData
{
    static_assert(chunkSizeXZ % CaveBiomeFields::downsample == 0,
                  "cave biome samples must align at chunk boundaries");
    static_assert(caveMaxY % 64 == 0);
    static constexpr uint32_t noiseSizeXZ = chunkSizeXZ / CaveBiomeFields::downsample + 1;
    static constexpr uint32_t wordsPerColumn = caveMaxY / 64;

    std::vector<uint64_t> airMask{};
    std::vector<float> noise{}; // temperature followed by humidity
    std::vector<CaveBiomeNoise> surfaceBias{};
    uint32_t noiseHeight = 0;

    void prepare()
    {
        airMask.assign(chunkSizeXZSquare * wordsPerColumn, 0);
        surfaceBias.resize(chunkSizeXZSquare);
    }

    void allocateNoise(uint32_t maxY)
    {
        // The extra Y planes enclose the last interpolation interval.
        noiseHeight = maxY / CaveBiomeFields::downsample + 2;
        noise.resize(2 * fieldSize());
    }

    uint32_t fieldSize() const { return noiseSizeXZ * noiseSizeXZ * noiseHeight; }
    float* temperatureNoise() { return noise.data(); }
    float* humidityNoise() { return noise.data() + fieldSize(); }

    CaveBiomeFields::Column temperatureColumn(uint32_t x, uint32_t z) const
    {
        return { noise.data(), noiseSizeXZ, noiseHeight, x, z };
    }

    CaveBiomeFields::Column humidityColumn(uint32_t x, uint32_t z) const
    {
        return { noise.data() + fieldSize(), noiseSizeXZ, noiseHeight, x, z };
    }

    void markCaveAir(uint32_t column, uint32_t y)
    {
        airMask[column * wordsPerColumn + y / 64] |= uint64_t(1) << (y % 64);
    }

    bool isCaveAir(uint32_t column, uint32_t y) const
    {
        return y < caveMaxY && ((airMask[column * wordsPerColumn + y / 64] >> (y % 64)) & 1);
    }

    void release() { *this = CaveDecorationData{}; }
};

// Per-column snow inputs computed during terrain generation and consumed by the structure pass:
// tree canopies can only be covered once they exist, and slopes need neighbor chunks' surfaces
struct SnowData
{
    // Surface rise per block (tan 35 degrees) at which a capped column is too steep to hold snow
    // and shows its rock instead
    static constexpr float capSteepGradient = 0.700f;
    // Steeper limit (tan 45 degrees) for snow layers on terrain, so the rock a too-steep cap exposes
    // still collects snow on its ledges
    static constexpr float layerSteepGradient = 1.0f;
    // Coverage ramps from none at a column's line to a continuous sheet this far above it, so snow
    // thins into patches downhill instead of ending at a hard edge
    static constexpr float fadeDepth = 6.f;
    // Where cover is partial, hollows hold snow and ridges shed it: this many blocks of
    // Chunk::terrainHollowness_WS shifts coverage by hollowBias, scaled down to nothing where cover
    // is none or complete.
    static constexpr float hollowScale = 4.f;
    static constexpr float hollowBias = 0.4f;

    // Coverage of a top block at topY before the hollowness bias: the altitude line's fade or the
    // cold-climate cover, whichever is more
    static float coverage(float lineY, float topY, float coldCover)
    {
        return glm::max(glm::smoothstep(lineY, lineY + fadeDepth, topY), coldCover);
    }

    // Layers rest only on full cubes
    static bool acceptsLayer(const BlockData& block)
    {
        return block.shape == BlockShape::CUBE &&
               (block.type == BlockType::SOLID || block.type == BlockType::TRANSPARENT_CUTOUT);
    }

    std::vector<float> lineY{};
    // Coverage that applies at any height, from cold climate alone
    std::vector<float> coldCover{};
    // [0, 1] noise a column's coverage must exceed; spatially coherent so partial cover forms
    // patches, and shared by every block in the column so canopies match the ground
    std::vector<float> patch{};
    // Set where the top-block stamp actually wrote snow. Read by the treeline checks during
    // generation and by the steep-rock swap; whatever block a capped top ends up as, it stays capped.
    std::vector<uint8_t> capped{};
    // Rock a too-steep cap exposes: the landform's surface rock, else stone
    std::vector<Block> exposedRock{};

    void prepare()
    {
        lineY.resize(chunkSizeXZSquare);
        coldCover.resize(chunkSizeXZSquare);
        patch.resize(chunkSizeXZSquare);
        capped.resize(chunkSizeXZSquare);
        exposedRock.resize(chunkSizeXZSquare);
    }

    void release() { *this = SnowData{}; }
};

class Chunk
{
public:
    static constexpr uint32_t structureNeighborSideLength = 2 * structureMaxChunkRadius + 1;
    static constexpr uint32_t numStructureNeighbors = structureNeighborSideLength * structureNeighborSideLength;
    // Row by row from the -X, -Z corner
    using ConstStructureNeighborhood = std::array<const Chunk*, numStructureNeighbors>;

private:
    const glm::ivec2 chunkPos;
    // Null for surface-only chunks, which are generated outside the region pipeline
    Region* const region;
    // Only seen from afar, so generated without caves, decorators or rock deep below the surface
    const bool isSurfaceOnly;

    std::vector<Block> blocks{};
    // One bit per block, set where the terrain pass left AIR. Captured before HAS_TERRAIN and never
    // written again, so neighbors may read it during their structure pass while this chunk's
    // blocks are being mutated. See knowledge/terrain/cave_structure_system.md.
    std::vector<uint64_t> terrainAirMask{};
    // One immutable bit per block identifying terrain full cubes. This permits race-free
    // support checks while neighboring chunks concurrently fill structures into air/water.
    std::vector<uint64_t> terrainSolidCubeMask{};
    CaveDecorationData caveDecoration{};
    SnowData snow{};
    // TODO: Consider replacing this unordered_map with a more cache-friendly sparse state store
    // if stateful blocks become common.
    std::unordered_map<uint32_t, uint8_t> blockStates{};
    std::vector<glm::uvec3> segmentsToGenerate{};

    std::vector<Biome> biomes{};
    // Highest solid terrain block per column (pre-structure). Lets later passes tell an
    // underground transition (cave floor) from the terrain surface.
    std::vector<uint16_t> terrainTopY{};
    // Sub-block terrain surface height per column, where terrain density crosses zero above the top
    // block, in 1/terrainSurfaceHeightScale blocks (0: no surface). Whole-block heights quantize a
    // gradient to multiples of 0.5, which lands on or next to a slope threshold and leaves speckles.
    // Like terrainTopY it is written once during generation, so neighbors read it for slopes.
    std::vector<uint16_t> terrainSurfaceHeight{};
    static constexpr float terrainSurfaceHeightScale = 64.f;
    static_assert(chunkSizeY * terrainSurfaceHeightScale <= 65535.f, "surface height must fit 16 bits");
    std::vector<Structure> structures{};
    std::vector<SurfaceStructureCandidate> surfaceStructureCandidates{};
    std::vector<CaveStructure> caveStructures{};
    // Only populated while this chunk fills its structures
    std::vector<const Chunk*> structureNeighbors{};
    // Bit per structure neighbor (see structureNeighborBit) that has terrain and has announced it.
    // Bits are set idempotently and cleared when a neighbor is removed, so neighbors may leave and
    // return; see knowledge/terrain/chunk_state_machine.md.
    std::atomic<uint32_t> readyStructureNeighborsMask{ 0 };

    std::array<Chunk*, 4> neighbors{};
    uint32_t numNeighborsSet{ 0 };
    // Bit per NeighborDirection whose chunk has all its blocks
    std::atomic<uint32_t> neighborsWithBlocksMask{ 0 };

    bool hasSerializedData{ false };

    std::atomic<ChunkState> state{ ChunkState::NEEDS_TERRAIN };
    // Main thread only
    bool isMarkedForDestruction{ false };
    bool areInstancesVisible{ false };

    Instance* terrainInstance{ nullptr };
    Instance* waterInstance{ nullptr };

    static_assert(numStructureNeighbors < 32, "readyStructureNeighborsMask needs a bit per structure neighbor");
    static constexpr uint32_t allStructureNeighborsMask = (1u << numStructureNeighbors) - 1;
    static constexpr uint32_t allNeighborsMask = (1u << 4) - 1;
    using StructureNeighborhood = std::array<Chunk*, numStructureNeighbors>;
    StructureNeighborhood collectStructureNeighbors();
    static uint32_t structureNeighborBit(glm::ivec2 offset);
    void markStructureNeighborsReady(uint32_t neighborBits);
    void markNeighborsWithBlocks(uint32_t neighborBits);

    // Returns the height from which every block it left is air
    uint32_t fillTerrainBlocksAndCreateStructures(ThreadMemoryAllocator& threadMemoryAlloc);
    void generateTerrainBlocks(ThreadMemoryAllocator& threadMemoryAlloc);
    // Every block from airFromY up must be air
    void buildTerrainAirMask(uint32_t airFromY = chunkSizeY);
    // The structure neighbor containing a world XZ position, and that position within it
    const Chunk* structureNeighborAt_WS(glm::ivec2 posXZ_WS, glm::ivec2& outPosXZ_CS) const;
    bool getTerrainMaskBit_WS(glm::ivec3 pos_WS, const std::vector<uint64_t> Chunk::* mask) const;
    void fillStructureBlocks(const Structure* structures, uint32_t numStructures);
    void placeSurfaceStructures();
    // Mean terrain height on rings around a column minus its own: positive in hollows, negative on
    // ridges. Reads neighbors' immutable terrain heights, so only valid during the structure pass.
    float terrainHollowness_WS(glm::ivec2 posXZ_WS) const;
    // Squared magnitude of the terrain surface gradient, by central differences across chunk borders.
    // Only valid during the structure pass.
    float terrainSlopeSquared_WS(glm::ivec2 posXZ_WS) const;
    void placeSnowLayers();
    // Places nothing when a two-tall block's upper cell is not air
    bool tryPlaceDecorator(uint32_t baseBlockIdx, uint32_t blockY, Block block);
    void fillCaveStructureBlocks(const CaveStructure* caveStructures, uint32_t numCaveStructures, CaveStructureType type);
    void runStructuresAndDecoratorPass();
    // Surface-only chunks have no cave air, so where a cave opens at the surface their plants differ from
    // the full chunk's
    void placeFloorDecorators();
    void placeCaveDecorators();
    void fillBlocksFromStructureNeighbors();

    bool shouldGenerateFace(glm::ivec3 thisPos_CS, BlockType thisBlockType, BlockShape thisBlockShape, glm::ivec3 neighborPos_CS, int faceIdx);

    bool isRegionAllBlockType(const glm::uvec3 startPos, const glm::uvec3 endPos, BlockType blockType, BlockShape blockShape = BlockShape::COUNT);
    bool isSegmentSurroundedBySolid(const glm::uvec3 startPos,
                                    const glm::uvec3 endPos,
                                    const glm::uvec3 chunkSegmentPos,
                                    const ChunkSegment* const prevSegments);

    void setNeighbor(NeighborDirection dir, Chunk* neighborChunk);

public:
    Chunk(glm::ivec2 chunkPos, Region* region, bool isSurfaceOnly = false);
    ~Chunk();

    // Surface-only chunks are generated outside the region pipeline, which tracks their readiness itself
    void generateSurfaceOnlyTerrain(ThreadMemoryAllocator& threadMemoryAlloc);
    // Every chunk in the neighborhood must have its terrain
    void fillSurfaceOnlyStructures(const ConstStructureNeighborhood& neighborhood);
    // Neighbors' structure passes read only a chunk's masks, heights and structures, so a surface-only
    // chunk downsampled already can give its blocks back while it stays a neighbor
    void releaseBlocks();

    void setNeighbors(bool createNeighbors);

    // Main thread only, while no task can touch this chunk or the removed one. Each undoes the
    // removed chunk's contribution to this chunk's readiness, stepping the state back to the
    // last stage that did not depend on it.
    void onNeighborRemoved(NeighborDirection dir);
    void onStructureNeighborRemoved(glm::ivec2 offset);

    void generateTerrain(ThreadMemoryAllocator& threadMemoryAlloc);
    void checkStructureNeighbors();
    void fillStructuresAndDecorators();
    void generateSegments(ThreadMemoryAllocator& threadMemoryAlloc);

    void setInstances(Instance* terrainInstance, Instance* waterInstance);
    void createInstances();
    void destroyInstances(ToFreeList& toFreeList);
    void cleanUnusedInstances(ToFreeList& toFreeList);
    Instance* getTerrainInstance() const;
    Instance* getWaterInstance() const;

    ChunkState getState() const;
    void setState(ChunkState newState);
    bool advanceState(ChunkState newState);

    bool getIsMarkedForDestruction() const;
    void setIsMarkedForDestruction(bool marked = true);

    void setInstancesVisible(bool visible);
    bool getAreInstancesVisible() const;
    // Has geometry with BLASes that it isn't about to lose, so it can be shown
    bool isGeometryReady() const;

    glm::ivec2 getChunkPos() const;
    Region* getRegion() const;

    Chunk* getNeighbor(NeighborDirection dir) const;
    uint32_t getNumNeighborsSet() const;
    bool getHasSerializedData() const;

    bool tryGetBlock(glm::uvec3 chunkBlockPos, Block& outBlock) const;
    // Whatever generation has written so far
    Block getGeneratedBlock(glm::uvec3 chunkBlockPos) const;
    // The column's generated blocks, bottom up
    const Block* getGeneratedColumn(glm::uvec2 chunkBlockPosXZ) const;

    const std::vector<Biome>& getBiomes() const;
    // Only valid once the chunk has all its blocks
    SerializedChunkView getSerializedView() const;
    uint64_t hashFinalBlocks() const;

    ChunkMemory getMemory() const;
    // Held for reuse after their chunks were destroyed
    static uint64_t getPooledBufferBytes();
    // For replacing the world, which does not generate into the pooled buffers
    static void clearBufferPool();

    void loadSerializedData(SerializedChunkData&& data);

    static uint32_t blockPosToIdx(glm::uvec3 chunkBlockPos);
    static uint32_t blockPosXZToIdx(glm::uvec2 chunkBlockPos);

    static uint32_t segmentPosToIdx(glm::uvec3 chunkSegmentPos);

    static void segmentPosToBounds(glm::uvec3 chunkSegmentPos, glm::uvec3& outSegmentStartPos, glm::uvec3& outSegmentEndPos);

    // Whether the terrain pass left AIR at a world position within this chunk's structure
    // neighborhood (radius structureMaxChunkRadius). Only valid during the structure pass.
    bool isTerrainAir_WS(glm::ivec3 pos_WS) const;
    bool isTerrainSolidCube_WS(glm::ivec3 pos_WS) const;

    static inline bool isInChunkXZ(glm::ivec2 pos_CS)
    {
        return glm::min(pos_CS.x, pos_CS.y /*z*/) >= 0 && glm::max(pos_CS.x, pos_CS.y /*z*/) < chunkSizeXZ;
    }

    static inline bool isInChunkXZ(glm::ivec3 pos_CS)
    {
        return isInChunkXZ(glm::ivec2(pos_CS.x, pos_CS.z));
    }

    static inline bool isInChunk(glm::ivec3 pos_CS)
    {
        return isInChunkXZ(pos_CS) && pos_CS.y >= 0 && pos_CS.y < chunkSizeY;
    }
};

class Region
{
private:
    std::array<Region*, 4> neighbors{};
    uint32_t numNeighborsSet{ 0 };
    // Far enough from the camera to be removed once unpinned; no new work is scheduled for it
    bool isStaged{ false };
    // Loaded from an imported world, so never evicted: regenerating it could differ from the import
    bool isImported{ false };
    // Queued or running tasks that may touch this region's chunks; it is only removed at zero
    std::atomic<uint32_t> numPins{ 0 };
    // Chunks found to have ready geometry, so that checking them again reads no chunk. BLAS builds don't
    // notify chunks, so bits are set when a check finds a chunk ready; a chunk only stops being ready
    // through destroyInstances or being marked for destruction, which clear its bit. Main thread only.
    std::bitset<regionSideLength * regionSideLength> readyGeometryChunks{};

public:
    const glm::ivec2 regionPos;
    const glm::ivec2 regionPosChunks;
    const glm::ivec2 regionMaxPosChunks; // inclusive

    std::array<std::unique_ptr<Chunk>, regionSideLength * regionSideLength> chunks{};

    Region(glm::ivec2 regionPos);

    Chunk* getChunk(glm::ivec2 chunkPos);
    Chunk* createChunk(glm::ivec2 chunkPos);
    Chunk* getOrCreateChunk(glm::ivec2 chunkPos);

    Region* getNeighbor(NeighborDirection dir) const;
    void setNeighbor(NeighborDirection dir, Region* neighborRegion);
    void clearNeighbor(NeighborDirection dir);
    uint32_t getNumNeighborsSet() const;

    bool containsChunk(glm::ivec2 chunkPos) const;

    bool isChunkGeometryReady(glm::ivec2 chunkPos);
    void clearChunkGeometryReady(glm::ivec2 chunkPos);

    void pin();
    void unpin();
    bool isPinned() const;

    bool getIsStaged() const;
    void setIsStaged(bool staged);
    bool getIsImported() const;
    void setIsImported();

    static uint32_t chunkPosToIdx(glm::ivec2 regionChunkPos);
};
