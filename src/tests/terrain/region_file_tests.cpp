// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "terrain/chunk.h"
#include "terrain/chunk_generator.h"
#include "terrain/region_file.h"
#include "multithreading/thread_memory_allocator.h"
#include "util/file_util.h"
#include "util/glm_util.h"

#include <json.hpp>
#include <lz4.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

extern uint32_t regionTestSeed;

namespace
{
void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::vector<Block> identityPalette()
{
    std::vector<Block> palette;
    for (uint32_t i = 0; i < static_cast<uint32_t>(Block::COUNT); ++i) palette.push_back(static_cast<Block>(i));
    return palette;
}

std::vector<char> bytesOf(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    check(static_cast<bool>(file), "read test file");
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

template<class T> T at(const std::vector<char>& bytes, size_t offset)
{
    check(offset + sizeof(T) <= bytes.size(), "test read bounds");
    T value;
    memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

template<class T> void put(std::vector<char>& bytes, size_t offset, T value)
{
    check(offset + sizeof(T) <= bytes.size(), "test write bounds");
    memcpy(bytes.data() + offset, &value, sizeof(value));
}

SerializedChunkData snapshot(const Chunk& chunk)
{
    return { chunk.getBlocks(), chunk.getBiomes(), chunk.getStructures(), chunk.getBlockStates(),
              chunk.getCaveStructures(), chunk.getTerrainAirMask(), chunk.getTerrainSolidCubeMask() };
}

void equalData(const Chunk& chunk, const SerializedChunkData& expected)
{
    check(chunk.getBlocks() == expected.blocks, "block round trip/boundary mismatch");
    check(chunk.getBiomes() == expected.biomes, "biome mismatch");
    check(chunk.getBlockStates() == expected.blockStates, "block-state mismatch");
    check(chunk.getTerrainAirMask() == expected.terrainAirMask, "original air mask mismatch");
    check(chunk.getTerrainSolidCubeMask() == expected.terrainSolidCubeMask, "original solid mask mismatch");
    const auto& structures = chunk.getStructures();
    check(structures.size() == expected.structures.size(), "surface count mismatch");
    for (size_t i = 0; i < structures.size(); ++i)
        check(structures[i].type == expected.structures[i].type && structures[i].pos_WS == expected.structures[i].pos_WS,
              "surface order/data mismatch");
    const auto& caves = chunk.getCaveStructures();
    check(caves.size() == expected.caveStructures.size(), "cave count mismatch");
    for (size_t i = 0; i < caves.size(); ++i)
        check(caves[i].type == expected.caveStructures[i].type && caves[i].pos_WS == expected.caveStructures[i].pos_WS &&
              caves[i].availableHeight == expected.caveStructures[i].availableHeight, "cave order/data mismatch");
}

SerializedChunkData fixture(glm::ivec2 chunkPos)
{
    SerializedChunkData data;
    data.blocks.assign(numChunkBlocks, Block::AIR);
    data.biomes.assign(chunkSizeXZSquare, Biome::FOREST);
    data.terrainAirMask.assign(numChunkBlocks / 64, ~uint64_t(0));
    data.terrainSolidCubeMask.assign(numChunkBlocks / 64, 0);
    // Final solids that were terrain air make rebuilding masks observably wrong.
    data.blocks[10] = Block::STONE;
    data.terrainAirMask[0] &= ~(uint64_t(1) << 10);
    data.terrainSolidCubeMask[0] |= uint64_t(1) << 10;
    data.blocks[11] = Block::LAMP;
    for (uint8_t face = 0; face < blockFaceCount; ++face)
    {
        data.blocks[50 + face] = Block::CRYSTAL_SHARD;
        data.blockStates.emplace(50 + face, face);
    }
    const glm::ivec3 origin(chunkPos.x * 16, 0, chunkPos.y * 16);
    data.structures = { { StructureType::CYPRESS_TREE, origin + glm::ivec3(15, 100, 0) },
                        { StructureType::OAK_TREE, origin + glm::ivec3(0, 99, 15) } };
    data.caveStructures = { { CaveStructureType::CAVE_VINES, origin + glm::ivec3(15, 50, 15), 23 },
                            { CaveStructureType::LAMP_CLUSTER, origin + glm::ivec3(0, 30, 0), 9 },
                            { CaveStructureType::STONE_COLUMN, origin + glm::ivec3(15, 1, 0), 511 } };
    return data;
}

void roundTripAndCorruption(const std::filesystem::path& directory)
{
    const glm::ivec2 position(-1, -1), chunkPos(-1, -1);
    Region source(position);
    auto expected = fixture(chunkPos);
    auto* chunk = source.createChunk(chunkPos);
    chunk->loadSerializedData(SerializedChunkData(expected));
    chunk->setState(ChunkState::HAS_ALL_BLOCKS);
    source.createChunk({ -2, -1 }); // unfinished: must not be saved
    const auto path = directory / "roundtrip.bin";
    uint32_t written = 0;
    check(RegionFile::write(path, source, written) && written == 1, "write completed chunks only");
    const auto original = bytesOf(path);
    check(at<uint16_t>(original, 4) == 7, "new format version");
    auto restored = RegionFile::read(path, position, identityPalette());
    check(restored && restored->getChunk({ -2, -1 }) == nullptr, "unfinished chunk survived");
    auto* loaded = restored->getChunk(chunkPos);
    check(loaded && loaded->getWasImported() && loaded->getState() == ChunkState::NEEDS_TERRAIN, "import lifecycle");
    equalData(*loaded, expected);
    ThreadMemoryAllocator scratch;
    loaded->generateTerrain(scratch);
    equalData(*loaded, expected); // must not rebuild masks
    loaded->fillStructuresAndDecorators();
    equalData(*loaded, expected); // imported blocks must not be decorated twice
    check(RegionFile::write(path, *restored, written), "rewrite existing file");
    check(bytesOf(path) == original, "deterministic re-export");

    size_t malformedIndex = 0;
    auto rejected = [&](std::vector<char> bytes) {
        const auto badPath = directory / ("malformed_" + std::to_string(malformedIndex++) + ".bin");
        check(FileUtil::writeAtomically(badPath, bytes), "write malformed fixture");
        check(!RegionFile::read(badPath, position, identityPalette()), "malformed region accepted");
    };
    for (size_t length : { size_t(0), size_t(15), size_t(16), size_t(37), original.size() - 1 })
        rejected({ original.begin(), original.begin() + length });
    auto bad = original; put<uint16_t>(bad, 4, 99); rejected(bad);
    bad = original; put<uint16_t>(bad, 14, 1025); rejected(bad);
    bad = original; put<uint16_t>(bad, 16, 1024); rejected(bad);
    bad = original; put<uint32_t>(bad, 18, UINT32_MAX); rejected(bad);
    bad = original; put<uint32_t>(bad, 26, numChunkBlocks + 1); rejected(bad);
    bad = original; put<uint32_t>(bad, 34, numChunkBlocks + 1); rejected(bad);
    bad = original; bad.push_back(0); rejected(bad);
    bad = original; put<uint16_t>(bad, 14, 2); bad.insert(bad.end(), original.begin() + 16, original.end()); rejected(bad);
    const size_t stateOffset = 38 + at<uint32_t>(original, 18) + at<uint32_t>(original, 22);
    bad = original; put<uint32_t>(bad, stateOffset, 50 | (6u << 17)); rejected(bad);
    bad = original; put<uint32_t>(bad, stateOffset + 4, at<uint32_t>(bad, stateOffset)); rejected(bad);
    check(!RegionFile::read(path, { 0, 0 }, identityPalette()), "region mismatch accepted");
    check(!RegionFile::read(path, { INT_MAX, 0 }, identityPalette()), "overflowing position accepted");

    // A write that cannot create its temporary file must not damage the previous file.
    std::filesystem::create_directory(path.string() + ".tmp");
    check(!RegionFile::write(path, source, written) && written == 0, "write failure ignored");
    check(bytesOf(path) == original, "failed write damaged previous region");
    std::filesystem::remove(path.string() + ".tmp");
    // Sharing without delete permission lets the temporary write finish but rejects
    // replacement. The writer must report failure and keep the old destination.
    const HANDLE lockedFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(lockedFile != INVALID_HANDLE_VALUE, "lock destination for failed publication");
    const bool published = RegionFile::write(path, source, written);
    CloseHandle(lockedFile);
    check(!published && written == 0, "rename failure ignored");
    check(bytesOf(path) == original && !std::filesystem::exists(path.string() + ".tmp"),
          "failed publication damaged original or leaked temporary file");

    Region empty({ 0, 0 });
    check(RegionFile::write(directory / "empty.bin", empty, written) && written == 0, "empty region write");
    check(RegionFile::read(directory / "empty.bin", { 0, 0 }, identityPalette()) != nullptr, "empty region read");

    // Derive old-format fixtures by removing the v7-only header/payload, preserving
    // the historical layout independently of the legacy reader.
    const size_t generationOffset = stateOffset + 6 * sizeof(uint32_t);
    std::vector<char> v6(original.begin(), original.begin() + 30);
    put<uint16_t>(v6, 4, 6);
    v6.insert(v6.end(), original.begin() + 38, original.begin() + generationOffset);
    check(FileUtil::writeAtomically(directory / "v6.bin", v6), "write v6 fixture");
    auto legacy6 = RegionFile::read(directory / "v6.bin", position, identityPalette());
    check(legacy6 != nullptr, "v6 load");
    check(legacy6->getChunk(chunkPos)->getBlockStates() == expected.blockStates, "v6 orientations lost");
    check(legacy6->getChunk(chunkPos)->getCaveStructures().empty(), "legacy cave approximation");
    legacy6->getChunk(chunkPos)->generateTerrain(scratch);
    check((legacy6->getChunk(chunkPos)->getTerrainAirMask()[0] & (uint64_t(1) << 11)) == 0, "legacy mask fallback");

    std::vector<char> v5(v6.begin(), v6.begin() + 26);
    put<uint16_t>(v5, 4, 5);
    v5.insert(v5.end(), v6.begin() + 30, v6.end() - 6 * sizeof(uint32_t));
    check(FileUtil::writeAtomically(directory / "v5.bin", v5), "write v5 fixture");
    auto legacy5 = RegionFile::read(directory / "v5.bin", position, identityPalette());
    check(legacy5 != nullptr, "v5 load");
    for (const auto& [index, face] : legacy5->getChunk(chunkPos)->getBlockStates())
        check(face == blockFaceIndex(BlockFace::Y_POS), "v5 floor orientation migration");
}

void goldenWorlds()
{
    size_t chunks = 0, files = 0;
    for (const auto& directory : std::filesystem::directory_iterator(std::filesystem::path(CMAKE_SOURCE_DIR) / "tests/voxel"))
    {
        std::ifstream manifest(directory.path() / "world.json");
        const auto json = nlohmann::json::parse(manifest);
        std::vector<Block> remap;
        for (const auto& name : json["blocks"])
        {
            const auto block = Blocks::fromId(name.get<std::string>());
            remap.push_back(block == Block::COUNT ? Block::MISSING : block);
        }
        for (const auto& entry : json["regions"])
        {
            const glm::ivec2 position(entry[0].get<int>(), entry[1].get<int>());
            auto region = RegionFile::read(directory.path() / RegionFile::fileName(position), position, remap);
            check(region != nullptr, "existing golden region failed to load");
            ++files;
            for (const auto& chunk : region->chunks) if (chunk) ++chunks;
        }
    }
    std::cout << "Legacy golden worlds: " << files << " regions, " << chunks << " chunks loaded\n";
}

using RegionMap = std::map<std::pair<int, int>, std::unique_ptr<Region>>;

Chunk* getChunk(RegionMap& regions, glm::ivec2 position)
{
    const glm::ivec2 regionPos = glmUtil::floorDiv(position, glm::ivec2(regionSideLength));
    auto& region = regions[{ regionPos.x, regionPos.y }];
    if (!region) region = std::make_unique<Region>(regionPos);
    return region->getOrCreateChunk(position);
}

void wire(RegionMap& regions)
{
    for (auto& [position, region] : regions)
        for (int direction : { 0, 1 })
        {
            const auto dir = static_cast<NeighborDirection>(direction);
            const glm::ivec2 neighbor = region->regionPos + neighborOffset(dir);
            const auto found = regions.find({ neighbor.x, neighbor.y });
            if (found != regions.end()) region->setNeighbor(dir, found->second.get());
        }
    for (auto& [position, region] : regions)
        for (auto& chunk : region->chunks) if (chunk) chunk->setNeighbors(false);
}

void boundaryGeneration(const std::filesystem::path& directory)
{
    for (uint32_t seed : { 4u, 96u, 1738u })
    {
        regionTestSeed = seed;
        ChunkGenerator::init();
        RegionMap original;
        ThreadMemoryAllocator scratch;
        // Cross both region seams at zero. A one-chunk halo supplies each 3x3 neighborhood.
        for (int z = -3; z <= 3; ++z)
            for (int x = -3; x <= 3; ++x) getChunk(original, { x, z });
        wire(original);
        for (int z = -3; z <= 3; ++z)
            for (int x = -3; x <= 3; ++x) { getChunk(original, { x, z })->generateTerrain(scratch); scratch.clear(); }
        // Force a lamp growth footprint across the saved/fresh seam in empty upper
        // air. This makes the boundary check exercise cave candidates on every seed,
        // independently of the naturally occurring cave distribution at the origin.
        auto& candidates = const_cast<std::vector<CaveStructure>&>(getChunk(original, { -1, 0 })->getCaveStructures());
        candidates.push_back({ CaveStructureType::LAMP_CLUSTER, { -1, 400, 8 }, 10 });
        for (int z = -2; z <= 2; ++z)
            for (int x = -2; x <= 2; ++x) getChunk(original, { x, z })->checkStructureNeighbors();
        // Export the completed western half while eastern neighbors are unfinished.
        for (int z = -1; z <= 1; ++z) getChunk(original, { -1, z })->fillStructuresAndDecorators();
        RegionMap restored;
        for (const auto& [position, region] : original)
        {
            const auto path = directory / RegionFile::fileName(region->regionPos);
            uint32_t written = 0;
            check(RegionFile::write(path, *region, written), "boundary export");
            restored[position] = RegionFile::read(path, region->regionPos, identityPalette());
            check(restored[position] != nullptr, "boundary import");
        }
        for (int z = -3; z <= 3; ++z)
            for (int x = -3; x <= 3; ++x) getChunk(restored, { x, z });
        wire(restored);
        for (int z = -3; z <= 3; ++z)
            for (int x = -3; x <= 3; ++x) { getChunk(restored, { x, z })->generateTerrain(scratch); scratch.clear(); }
        for (int z = -2; z <= 2; ++z)
            for (int x = -2; x <= 2; ++x) getChunk(restored, { x, z })->checkStructureNeighbors();
        for (int z = -1; z <= 1; ++z) getChunk(restored, { -1, z })->fillStructuresAndDecorators();
        // Opposite orders must still produce the same data at the imported/fresh boundary.
        for (int z = -1; z <= 1; ++z) getChunk(original, { 0, z })->fillStructuresAndDecorators();
        for (int z = 1; z >= -1; --z) getChunk(restored, { 0, z })->fillStructuresAndDecorators();
        bool lampCrossedBoundary = false;
        for (uint32_t z = 0; z < chunkSizeXZ; ++z)
            for (uint32_t x = 0; x < chunkSizeXZ; ++x)
                for (uint32_t y = 394; y <= 403; ++y)
                    lampCrossedBoundary |= getChunk(original, { 0, 0 })->getBlocks()[Chunk::blockPosToIdx({ x, y, z })] == Block::LAMP;
        check(lampCrossedBoundary, "fixture did not exercise a cave structure crossing the export boundary");
        for (int z = -1; z <= 1; ++z)
            for (int x = -1; x <= 0; ++x)
                equalData(*getChunk(restored, { x, z }), snapshot(*getChunk(original, { x, z })));
        std::cout << "Imported/fresh boundary matches for seed " << seed << '\n';
    }
}
} // namespace

int main()
{
    try
    {
        const auto directory = std::filesystem::path(CMAKE_BINARY_DIR) / "region_file_tests";
        std::filesystem::create_directories(directory);
        Blocks::init(); Biomes::init(); CaveBiomes::init(); Structures::init(); CaveStructures::init();
        roundTripAndCorruption(directory);
        goldenWorlds();
        boundaryGeneration(directory);
        std::cout << "Region file tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
