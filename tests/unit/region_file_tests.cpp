#include "terrain/region_file.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

namespace
{

constexpr glm::ivec2 regionPos{ 1, -2 };
const glm::ivec2 regionOrigin = regionPos * static_cast<int>(regionSideLength);

// The codec only reads state kinds, so the test marks an arbitrary block as surface-mounted
// instead of loading block assets.
constexpr Block surfaceMountBlock = Block::GRASS;

struct TestRegistry
{
    std::vector<BlockStateKind> blockStateKinds;
    StructureGen surfaceGen{ StructureType::OAK_TREE, 8 };
    SurfaceStructureGens surfaceGens;

    TestRegistry() : blockStateKinds(static_cast<size_t>(Block::COUNT), BlockStateKind::NONE)
    {
        blockStateKinds[static_cast<size_t>(surfaceMountBlock)] = BlockStateKind::SURFACE_MOUNT;
        surfaceGen.surfacePlacement = StructureSurfacePlacement{ .id = SurfaceStructureGenId::TIANZI_PINES };
        surfaceGens.emplace(SurfaceStructureGenId::TIANZI_PINES, &surfaceGen);
    }

    RegionFile::Registry get() const
    {
        return { blockStateKinds, &surfaceGens };
    }
};

// Unique per test case so parallel CTest runs never share files.
class TempDir
{
    std::filesystem::path path;

public:
    explicit TempDir(const std::string& name)
        : path(std::filesystem::temp_directory_path() / ("biomeinator_region_file_tests_" + name))
    {
        std::filesystem::remove_all(this->path);
        std::filesystem::create_directories(this->path);
    }

    ~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(this->path, error);
    }

    std::filesystem::path regionPath() const
    {
        return this->path / RegionFile::fileName(regionPos);
    }
};

SerializedChunkData makeChunk(glm::ivec2 chunkPos, const TestRegistry& registry, uint32_t seed)
{
    std::mt19937 rng(seed);
    const glm::ivec2 origin = chunkPos * static_cast<int>(chunkSizeXZ);
    SerializedChunkData data;

    data.blocks.resize(numChunkBlocks);
    for (uint32_t index = 0; index < numChunkBlocks; ++index)
    {
        const uint32_t roll = rng() % 16;
        data.blocks[index] = roll == 0 ? surfaceMountBlock : (roll < 8 ? Block::STONE : Block::AIR);
        if (data.blocks[index] == surfaceMountBlock)
        {
            data.blockStates.emplace(index, static_cast<uint8_t>(rng() % blockFaceCount));
        }
    }
    data.biomes.resize(chunkSizeXZSquare);
    for (Biome& biome : data.biomes)
    {
        biome = static_cast<Biome>(rng() % static_cast<uint32_t>(Biome::COUNT));
    }

    data.terrainAirMask.resize(numChunkBlocks / 64);
    data.terrainSolidCubeMask.resize(numChunkBlocks / 64);
    for (size_t word = 0; word < data.terrainAirMask.size(); ++word)
    {
        const uint64_t bits = (static_cast<uint64_t>(rng()) << 32) | rng();
        data.terrainAirMask[word] = bits;
        data.terrainSolidCubeMask[word] = ~bits & (static_cast<uint64_t>(rng()) << 32);
    }
    data.terrainTopY.resize(chunkSizeXZSquare);
    data.terrainSurfaceHeight.resize(chunkSizeXZSquare);
    for (uint32_t column = 0; column < chunkSizeXZSquare; ++column)
    {
        data.terrainTopY[column] = static_cast<uint16_t>(rng() % chunkSizeY);
        data.terrainSurfaceHeight[column] = static_cast<uint16_t>(rng());
    }

    // Deliberately not sorted: candidate order is fill precedence and must survive.
    data.structures = {
        { StructureType::PALM_TREE, { origin.x + 9, 70, origin.y + 2 } },
        { StructureType::OAK_TREE, { origin.x + 1, 65, origin.y + 15 } },
    };
    data.caveStructures = {
        { CaveStructureType::STONE_COLUMN, { origin.x + 15, 40, origin.y + 0 }, 12 },
        { CaveStructureType::LAMP_CLUSTER, { origin.x + 3, 200, origin.y + 7 }, static_cast<int>(chunkSizeY) },
    };
    data.surfaceStructureCandidates = {
        { { origin.x + 5, 90, origin.y + 5 }, &registry.surfaceGen, 7, 30 },
        { { origin.x + 4, chunkSizeY - 1, origin.y + 11 }, &registry.surfaceGen, 3, 1 },
    };
    return data;
}

SerializedChunkView viewOf(glm::ivec2 position, const SerializedChunkData& data)
{
    return {
        .position = position,
        .blocks = data.blocks,
        .biomes = data.biomes,
        .structures = data.structures,
        .blockStates = &data.blockStates,
        .caveStructures = data.caveStructures,
        .surfaceStructureCandidates = data.surfaceStructureCandidates,
        .terrainAirMask = data.terrainAirMask,
        .terrainSolidCubeMask = data.terrainSolidCubeMask,
        .terrainTopY = data.terrainTopY,
        .terrainSurfaceHeight = data.terrainSurfaceHeight,
    };
}

std::vector<Block> identityRemap()
{
    std::vector<Block> remap(static_cast<size_t>(Block::COUNT));
    for (size_t index = 0; index < remap.size(); ++index)
    {
        remap[index] = static_cast<Block>(index);
    }
    return remap;
}

std::vector<char> readBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

void writeBytes(const std::filesystem::path& path, const std::vector<char>& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void requireEqual(const SerializedChunkData& actual, const SerializedChunkData& expected)
{
    CHECK(actual.blocks == expected.blocks);
    CHECK(actual.biomes == expected.biomes);
    CHECK(actual.blockStates == expected.blockStates);
    CHECK(actual.terrainAirMask == expected.terrainAirMask);
    CHECK(actual.terrainSolidCubeMask == expected.terrainSolidCubeMask);
    CHECK(actual.terrainTopY == expected.terrainTopY);
    CHECK(actual.terrainSurfaceHeight == expected.terrainSurfaceHeight);

    REQUIRE(actual.structures.size() == expected.structures.size());
    for (size_t index = 0; index < expected.structures.size(); ++index)
    {
        CHECK(actual.structures[index].type == expected.structures[index].type);
        CHECK(actual.structures[index].pos_WS == expected.structures[index].pos_WS);
    }
    REQUIRE(actual.caveStructures.size() == expected.caveStructures.size());
    for (size_t index = 0; index < expected.caveStructures.size(); ++index)
    {
        CHECK(actual.caveStructures[index].type == expected.caveStructures[index].type);
        CHECK(actual.caveStructures[index].pos_WS == expected.caveStructures[index].pos_WS);
        CHECK(actual.caveStructures[index].availableHeight == expected.caveStructures[index].availableHeight);
    }
    REQUIRE(actual.surfaceStructureCandidates.size() == expected.surfaceStructureCandidates.size());
    for (size_t index = 0; index < expected.surfaceStructureCandidates.size(); ++index)
    {
        const SurfaceStructureCandidate& a = actual.surfaceStructureCandidates[index];
        const SurfaceStructureCandidate& e = expected.surfaceStructureCandidates[index];
        CHECK(a.pos_WS == e.pos_WS);
        CHECK(a.gen == e.gen);
        CHECK(a.priority == e.priority);
        CHECK(a.headroom == e.headroom);
    }
}

} // namespace

TEST_CASE("RegionFile v7 round-trips every generation input in order", "[unit][region_file]")
{
    const TestRegistry registry;
    const TempDir dir("round_trip");
    const std::vector<glm::ivec2> positions{ regionOrigin + glm::ivec2(31, 31), regionOrigin + glm::ivec2(0, 4) };
    const std::vector<SerializedChunkData> chunks{ makeChunk(positions[0], registry, 1),
                                                   makeChunk(positions[1], registry, 2) };
    const std::vector<SerializedChunkView> views{ viewOf(positions[0], chunks[0]), viewOf(positions[1], chunks[1]) };
    REQUIRE(RegionFile::write(dir.regionPath(), regionPos, views, registry.get()));

    const auto decoded = RegionFile::read(dir.regionPath(), regionPos, identityRemap(), registry.get());
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 2);
    // Chunks are stored by region index, so the chunk at local (0, 4) comes first.
    CHECK((*decoded)[0].position == positions[1]);
    CHECK((*decoded)[1].position == positions[0]);
    requireEqual((*decoded)[0].data, chunks[1]);
    requireEqual((*decoded)[1].data, chunks[0]);
}

TEST_CASE("RegionFile re-export of decoded data is byte-identical", "[unit][region_file]")
{
    const TestRegistry registry;
    const TempDir dir("reexport");
    const glm::ivec2 position = regionOrigin + glm::ivec2(7, 3);
    const SerializedChunkData chunk = makeChunk(position, registry, 3);
    const std::vector<SerializedChunkView> views{ viewOf(position, chunk) };
    REQUIRE(RegionFile::write(dir.regionPath(), regionPos, views, registry.get()));
    const std::vector<char> original = readBytes(dir.regionPath());

    const auto decoded = RegionFile::read(dir.regionPath(), regionPos, identityRemap(), registry.get());
    REQUIRE(decoded);
    const std::vector<SerializedChunkView> decodedViews{ viewOf(position, (*decoded)[0].data) };
    REQUIRE(RegionFile::write(dir.regionPath(), regionPos, decodedViews, registry.get()));
    CHECK(readBytes(dir.regionPath()) == original);
}

TEST_CASE("RegionFile writes missing legacy terrain heights as no surface", "[unit][region_file]")
{
    const TestRegistry registry;
    const TempDir dir("legacy_heights");
    const glm::ivec2 position = regionOrigin;
    SerializedChunkData chunk = makeChunk(position, registry, 4);
    chunk.terrainTopY.clear();
    chunk.terrainSurfaceHeight.clear();
    const std::vector<SerializedChunkView> views{ viewOf(position, chunk) };
    REQUIRE(RegionFile::write(dir.regionPath(), regionPos, views, registry.get()));

    const auto decoded = RegionFile::read(dir.regionPath(), regionPos, identityRemap(), registry.get());
    REQUIRE(decoded);
    const SerializedChunkData& data = (*decoded)[0].data;
    CHECK(data.terrainTopY == std::vector<uint16_t>(chunkSizeXZSquare, 0));
    CHECK(data.terrainSurfaceHeight == std::vector<uint16_t>(chunkSizeXZSquare, 0));
}

TEST_CASE("RegionFile rejects surface generators outside the registry", "[unit][region_file]")
{
    const TestRegistry registry;
    const TempDir dir("generators");
    const glm::ivec2 position = regionOrigin;
    const SerializedChunkData chunk = makeChunk(position, registry, 5);
    const std::vector<SerializedChunkView> views{ viewOf(position, chunk) };

    const SurfaceStructureGens emptyGens;
    const RegionFile::Registry withoutGens{ registry.blockStateKinds, &emptyGens };
    CHECK_FALSE(RegionFile::write(dir.regionPath(), regionPos, views, withoutGens));
    CHECK_FALSE(std::filesystem::exists(dir.regionPath()));

    REQUIRE(RegionFile::write(dir.regionPath(), regionPos, views, registry.get()));
    CHECK_FALSE(RegionFile::read(dir.regionPath(), regionPos, identityRemap(), withoutGens));
}

TEST_CASE("RegionFile rejects inconsistent block states", "[unit][region_file]")
{
    const TestRegistry registry;
    const TempDir dir("block_states");
    const glm::ivec2 position = regionOrigin;
    SerializedChunkData chunk = makeChunk(position, registry, 6);
    REQUIRE_FALSE(chunk.blockStates.empty());
    chunk.blockStates.erase(chunk.blockStates.begin());
    const std::vector<SerializedChunkView> views{ viewOf(position, chunk) };
    CHECK_FALSE(RegionFile::write(dir.regionPath(), regionPos, views, registry.get()));
}

TEST_CASE("RegionFile rejects truncated, padded, and misplaced files", "[unit][region_file]")
{
    const TestRegistry registry;
    const TempDir dir("corruption");
    const glm::ivec2 position = regionOrigin + glm::ivec2(2, 2);
    const SerializedChunkData chunk = makeChunk(position, registry, 7);
    const std::vector<SerializedChunkView> views{ viewOf(position, chunk) };
    REQUIRE(RegionFile::write(dir.regionPath(), regionPos, views, registry.get()));
    const std::vector<char> original = readBytes(dir.regionPath());

    CHECK_FALSE(RegionFile::read(dir.regionPath(), regionPos + glm::ivec2(1, 0), identityRemap(), registry.get()));

    std::vector<char> truncated(original.begin(), original.end() - 1);
    writeBytes(dir.regionPath(), truncated);
    CHECK_FALSE(RegionFile::read(dir.regionPath(), regionPos, identityRemap(), registry.get()));

    std::vector<char> padded = original;
    padded.push_back(0);
    writeBytes(dir.regionPath(), padded);
    CHECK_FALSE(RegionFile::read(dir.regionPath(), regionPos, identityRemap(), registry.get()));
}
