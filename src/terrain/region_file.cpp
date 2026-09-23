// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "region_file.h"

#include "chunk.h"
#include "logger.h"
#include "util/file_util.h"

#include <lz4.h>

#include <algorithm>
#include <bitset>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace RegionFile
{
namespace
{
constexpr uint32_t magic = 0x42494F4D;
constexpr uint16_t currentVersion = 7;
constexpr uint16_t blockStatesVersion = 6;
constexpr uint16_t generationDataVersion = 7;
constexpr uint16_t oldestVersion = 5;
constexpr size_t regionChunkCount = regionSideLength * regionSideLength;
constexpr size_t blockBiomeBytes = numChunkBlocks * sizeof(Block) + chunkSizeXZSquare * sizeof(Biome);
constexpr size_t maskWords = numChunkBlocks / 64;
constexpr size_t maskBytes = maskWords * sizeof(uint64_t);
constexpr size_t structureBytesLimit = sizeof(uint32_t) * (1 + maxSurfaceStructuresPerChunk);
// At most one candidate per floor/ceiling event; this is a conservative voxel-count bound.
constexpr uint32_t maxCaveStructures = numChunkBlocks;
constexpr uint32_t blockStateIndexBits = 17;
constexpr uint32_t blockStateIndexMask = (1u << blockStateIndexBits) - 1;

static_assert(sizeof(Block) == 2 && sizeof(Biome) == 1);
static_assert(numChunkBlocks == (1u << blockStateIndexBits));
static_assert(chunkSizeXZ == 16 && chunkSizeY == 512);
static_assert(static_cast<size_t>(StructureType::COUNT) <= 256);
static_assert(static_cast<size_t>(CaveStructureType::COUNT) <= 256);

// Little-endian Windows format. Region header: magic(u32), version(u16), x/z(i32),
// chunk count(u16). v5 chunk header: local index(u16), blocks/structures LZ4 sizes(u32).
// v6 adds block-state count(u32). v7 adds generation LZ4 size and cave count(u32).
// Payload order: LZ4(blocks + biomes), LZ4(surface candidates), packed block states,
// then v7 LZ4(air mask + solid-cube mask + ordered cave candidates).
// Surface candidate payload starts with its count(u32). Each candidate's type/position
// is packed [type:8, x:4, y:9, z:4, unused:7]; cave records append availableHeight(u32).

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void append(std::vector<char>& bytes, const void* data, size_t size)
{
    if (size == 0)
    {
        return;
    }
    const char* start = static_cast<const char*>(data);
    bytes.insert(bytes.end(), start, start + size);
}

template<class T>
void append(std::vector<char>& bytes, T value)
{
    append(bytes, &value, sizeof(value));
}

uint32_t compress(std::vector<char>& bytes, const std::vector<char>& input)
{
    require(input.size() <= LZ4_MAX_INPUT_SIZE, "payload too large for LZ4");
    const size_t offset = bytes.size();
    const int capacity = LZ4_compressBound(static_cast<int>(input.size()));
    bytes.resize(offset + capacity);
    const int size = LZ4_compress_default(input.data(), bytes.data() + offset,
                                         static_cast<int>(input.size()), capacity);
    require(size > 0, "LZ4 compression failed");
    bytes.resize(offset + size);
    return static_cast<uint32_t>(size);
}

class Reader
{
    std::istream& file;
    uint64_t remaining;

public:
    Reader(std::istream& file, uint64_t size) : file(file), remaining(size) {}

    void bytes(void* destination, size_t size)
    {
        require(size <= remaining, "payload runs past EOF");
        if (size != 0)
        {
            file.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
        }
        require(static_cast<bool>(file), "file read failed");
        remaining -= size;
    }

    template<class T>
    T value()
    {
        T result;
        bytes(&result, sizeof(result));
        return result;
    }

    std::vector<char> compressed(uint32_t size, size_t outputSize, bool exact = true)
    {
        require(size > 0 && size <= remaining && size <= LZ4_compressBound(static_cast<int>(outputSize)),
                "invalid compressed payload size");
        std::vector<char> input(size);
        bytes(input.data(), input.size());
        std::vector<char> output(outputSize);
        const int decoded = LZ4_decompress_safe(input.data(), output.data(), static_cast<int>(size),
                                                static_cast<int>(outputSize));
        require(decoded > 0 && (!exact || decoded == outputSize), "invalid LZ4 payload");
        output.resize(decoded);
        return output;
    }

    void finish() const
    {
        require(remaining == 0, "unexpected trailing region data");
    }
};

uint32_t wordAt(const std::vector<char>& bytes, size_t offset)
{
    require(offset <= bytes.size() && sizeof(uint32_t) <= bytes.size() - offset, "truncated candidate data");
    uint32_t word;
    memcpy(&word, bytes.data() + offset, sizeof(word));
    return word;
}

uint32_t packCandidate(uint32_t type, glm::ivec3 position, glm::ivec2 chunkOrigin, uint32_t typeCount)
{
    const int64_t x = static_cast<int64_t>(position.x) - chunkOrigin.x;
    const int64_t z = static_cast<int64_t>(position.z) - chunkOrigin.y;
    require(type < typeCount && x >= 0 && x < chunkSizeXZ && z >= 0 && z < chunkSizeXZ &&
            position.y >= 0 && position.y < chunkSizeY, "invalid structure candidate");
    return type | (static_cast<uint32_t>(x) << 8) | (static_cast<uint32_t>(position.y) << 12) |
           (static_cast<uint32_t>(z) << 21);
}

glm::ivec3 candidatePosition(uint32_t packed, glm::ivec2 chunkOrigin, uint32_t typeCount)
{
    require((packed >> 25) == 0 && (packed & 0xffu) < typeCount, "invalid structure type or packed bits");
    return { chunkOrigin.x + static_cast<int>((packed >> 8) & 0xfu),
             static_cast<int>((packed >> 12) & 0x1ffu),
             chunkOrigin.y + static_cast<int>((packed >> 21) & 0xfu) };
}

void validateBlocks(const std::vector<Block>& blocks, const std::unordered_map<uint32_t, uint8_t>& states)
{
    require(states.size() <= numChunkBlocks, "too many block states");
    for (const auto& [index, state] : states)
    {
        require(index < blocks.size() && state < blockFaceCount && blocks[index] < Block::COUNT &&
                Blocks::getBlockData(blocks[index]).stateKind == BlockStateKind::SURFACE_MOUNT,
                "invalid block state");
    }
    for (uint32_t index = 0; index < blocks.size(); ++index)
    {
        require(blocks[index] < Block::COUNT, "invalid block");
        require(Blocks::getBlockData(blocks[index]).stateKind != BlockStateKind::SURFACE_MOUNT || states.contains(index),
                "missing surface-mount block state");
    }
}
} // namespace

std::string fileName(glm::ivec2 regionPos)
{
    return "region_" + std::to_string(regionPos.x) + "_" + std::to_string(regionPos.y) + ".bin";
}

bool isValidPosition(glm::ivec2 regionPos)
{
    // All block positions in the region must fit the engine's signed world coordinates.
    constexpr int64_t side = regionSideLength * chunkSizeXZ;
    for (int axis = 0; axis < 2; ++axis)
    {
        const int64_t start = static_cast<int64_t>(regionPos[axis]) * side;
        if (start < std::numeric_limits<int32_t>::min() || start + side - 1 > std::numeric_limits<int32_t>::max())
        {
            return false;
        }
    }
    return true;
}

bool write(const std::filesystem::path& path, glm::ivec2 position, std::span<const Chunk* const> inputChunks)
{
    try
    {
        require(isValidPosition(position), "region coordinates out of range");
        require(inputChunks.size() <= regionChunkCount, "too many chunks");
        const glm::ivec2 regionOrigin = position * static_cast<int>(regionSideLength);
        std::vector<std::pair<uint16_t, const Chunk*>> chunks;
        chunks.reserve(inputChunks.size());
        std::bitset<regionChunkCount> seen;
        for (const Chunk* chunk : inputChunks)
        {
            require(chunk && chunk->getState() >= ChunkState::HAS_ALL_BLOCKS, "unfinished chunk");
            const int64_t localX = static_cast<int64_t>(chunk->getChunkPos().x) - regionOrigin.x;
            const int64_t localZ = static_cast<int64_t>(chunk->getChunkPos().y) - regionOrigin.y;
            require(localX >= 0 && localX < regionSideLength && localZ >= 0 && localZ < regionSideLength,
                    "chunk outside region");
            const uint16_t index = static_cast<uint16_t>(localX + localZ * regionSideLength);
            require(!seen[index], "duplicate chunk");
            seen.set(index);
            chunks.emplace_back(index, chunk);
        }
        std::sort(chunks.begin(), chunks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        return FileUtil::writeAtomically(path, [&](std::ostream& file)
        {
            std::vector<char> bytes;
            append(bytes, magic);
            append(bytes, currentVersion);
            append(bytes, static_cast<int32_t>(position.x));
            append(bytes, static_cast<int32_t>(position.y));
            append(bytes, static_cast<uint16_t>(chunks.size()));
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            std::vector<char> payload;
            for (const auto& [index, chunk] : chunks)
            {
                // Reuse scratch for one encoded chunk; never retain a whole region's output.
                bytes.clear();
                const auto& blocks = chunk->getBlocks();
                const auto& biomes = chunk->getBiomes();
                const auto& air = chunk->getTerrainAirMask();
                const auto& solid = chunk->getTerrainSolidCubeMask();
                require(blocks.size() == numChunkBlocks && biomes.size() == chunkSizeXZSquare &&
                        air.size() == maskWords && solid.size() == maskWords, "incomplete chunk data");
                for (Biome biome : biomes)
                {
                    require(biome < Biome::COUNT, "invalid biome");
                }
                for (size_t word = 0; word < maskWords; ++word)
                {
                    require((air[word] & solid[word]) == 0, "overlapping terrain masks");
                }
                validateBlocks(blocks, chunk->getBlockStates());
                const glm::ivec2 origin = chunk->getChunkPos() * static_cast<int>(chunkSizeXZ);

                append(bytes, index);
                const size_t headerOffset = bytes.size();
                bytes.resize(headerOffset + 5 * sizeof(uint32_t));
                payload.clear();
                append(payload, blocks.data(), blocks.size() * sizeof(Block));
                append(payload, biomes.data(), biomes.size() * sizeof(Biome));
                const uint32_t compressedBlocks = compress(bytes, payload);

                uint32_t compressedStructures = 0;
                const auto& structures = chunk->getStructures();
                require(structures.size() <= maxSurfaceStructuresPerChunk, "too many surface candidates");
                if (!structures.empty())
                {
                    payload.clear();
                    append(payload, static_cast<uint32_t>(structures.size()));
                    for (const Structure& structure : structures)
                    {
                        append(payload, packCandidate(static_cast<uint32_t>(structure.type), structure.pos_WS, origin,
                                                      static_cast<uint32_t>(StructureType::COUNT)));
                    }
                    compressedStructures = compress(bytes, payload);
                }

                const auto& states = chunk->getBlockStates();
                std::vector<std::pair<uint32_t, uint8_t>> sortedStates(states.begin(), states.end());
                std::sort(sortedStates.begin(), sortedStates.end());
                for (const auto& [blockIndex, state] : sortedStates)
                {
                    append(bytes, blockIndex | (static_cast<uint32_t>(state) << blockStateIndexBits));
                }

                payload.clear();
                append(payload, air.data(), maskBytes);
                append(payload, solid.data(), maskBytes);
                const auto& caves = chunk->getCaveStructures();
                require(caves.size() <= maxCaveStructures, "too many cave candidates");
                for (const CaveStructure& cave : caves)
                {
                    append(payload, packCandidate(static_cast<uint32_t>(cave.type), cave.pos_WS, origin,
                                                   static_cast<uint32_t>(CaveStructureType::COUNT)));
                    require(cave.availableHeight > 0 && cave.availableHeight <= chunkSizeY, "invalid cave height");
                    append(payload, static_cast<uint32_t>(cave.availableHeight));
                }
                const uint32_t compressedGeneration = compress(bytes, payload);
                const uint32_t header[] = { compressedBlocks, compressedStructures, static_cast<uint32_t>(states.size()),
                                            compressedGeneration, static_cast<uint32_t>(caves.size()) };
                memcpy(bytes.data() + headerOffset, header, sizeof(header));
                file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                require(static_cast<bool>(file), "chunk write failed");
            }
        });
    }
    catch (const std::exception& error)
    {
        Logger::logError("region export: %s: %s", path.generic_string().c_str(), error.what());
        return false;
    }
}

std::optional<DecodedRegion> read(const std::filesystem::path& path, glm::ivec2 expectedPos,
                                  std::span<const Block> blockRemap)
{
    try
    {
        require(isValidPosition(expectedPos), "region coordinates out of range");
        require(!blockRemap.empty() && blockRemap.size() <= (1u << 16), "invalid block palette size");
        for (Block block : blockRemap)
        {
            require(block < Block::COUNT, "invalid block palette entry");
        }
        FileUtil::PathLock lock(path);
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        require(static_cast<bool>(file), "failed to open region");
        const auto size = file.tellg();
        require(size >= 0, "failed to read region size");
        file.seekg(0);
        Reader reader(file, static_cast<uint64_t>(size));
        require(reader.value<uint32_t>() == magic, "bad region magic");
        const uint16_t version = reader.value<uint16_t>();
        require(version >= oldestVersion && version <= currentVersion, "unsupported region version");
        const int32_t x = reader.value<int32_t>();
        const int32_t z = reader.value<int32_t>();
        require(glm::ivec2(x, z) == expectedPos, "region coordinates do not match filename");
        const uint16_t count = reader.value<uint16_t>();
        require(count <= regionChunkCount, "invalid region chunk count");
        DecodedRegion chunks;
        chunks.reserve(count);
        const glm::ivec2 regionOrigin = expectedPos * static_cast<int>(regionSideLength);
        std::bitset<regionChunkCount> seen;
        for (uint16_t i = 0; i < count; ++i)
        {
            const uint16_t index = reader.value<uint16_t>();
            require(index < regionChunkCount && !seen[index], "invalid or duplicate chunk index");
            seen.set(index);
            const uint32_t compressedBlocks = reader.value<uint32_t>();
            const uint32_t compressedStructures = reader.value<uint32_t>();
            const uint32_t stateCount = version >= blockStatesVersion ? reader.value<uint32_t>() : 0;
            const uint32_t compressedGeneration = version >= generationDataVersion ? reader.value<uint32_t>() : 0;
            const uint32_t caveCount = version >= generationDataVersion ? reader.value<uint32_t>() : 0;
            require(stateCount <= numChunkBlocks && caveCount <= maxCaveStructures, "invalid chunk payload count");

            SerializedChunkData data;
            const auto payload = reader.compressed(compressedBlocks, blockBiomeBytes);
            data.blocks.resize(numChunkBlocks);
            data.biomes.resize(chunkSizeXZSquare);
            memcpy(data.blocks.data(), payload.data(), numChunkBlocks * sizeof(Block));
            memcpy(data.biomes.data(), payload.data() + numChunkBlocks * sizeof(Block), chunkSizeXZSquare * sizeof(Biome));
            for (Block& block : data.blocks)
            {
                const size_t paletteIndex = static_cast<size_t>(block);
                require(paletteIndex < blockRemap.size(), "block exceeds palette size");
                block = blockRemap[paletteIndex];
            }
            for (Biome biome : data.biomes)
            {
                require(biome < Biome::COUNT, "invalid biome");
            }

            const glm::ivec2 chunkPos = regionOrigin + glm::ivec2(index % regionSideLength, index / regionSideLength);
            const glm::ivec2 origin = chunkPos * static_cast<int>(chunkSizeXZ);
            if (compressedStructures != 0)
            {
                const auto candidates = reader.compressed(compressedStructures, structureBytesLimit, false);
                const uint32_t structureCount = wordAt(candidates, 0);
                require(structureCount <= maxSurfaceStructuresPerChunk &&
                        candidates.size() == sizeof(uint32_t) * (1 + structureCount),
                        "invalid surface candidate count");
                for (uint32_t s = 0; s < structureCount; ++s)
                {
                    const uint32_t packed = wordAt(candidates, sizeof(uint32_t) * (1 + s));
                    const auto position = candidatePosition(packed, origin, static_cast<uint32_t>(StructureType::COUNT));
                    data.structures.push_back({ static_cast<StructureType>(packed & 0xffu), position });
                }
            }
            for (uint32_t s = 0; s < stateCount; ++s)
            {
                const uint32_t packed = reader.value<uint32_t>();
                const uint32_t blockIndex = packed & blockStateIndexMask;
                const uint32_t state = packed >> blockStateIndexBits;
                require(state < blockFaceCount, "invalid packed block state");
                require(data.blockStates.emplace(blockIndex, static_cast<uint8_t>(state)).second, "duplicate block state");
            }
            if (version == oldestVersion)
            {
                for (uint32_t blockIndex = 0; blockIndex < numChunkBlocks; ++blockIndex)
                {
                    if (Blocks::getBlockData(data.blocks[blockIndex]).stateKind == BlockStateKind::SURFACE_MOUNT)
                    {
                        data.blockStates.emplace(blockIndex, blockFaceIndex(BlockFace::Y_POS));
                    }
                }
            }
            validateBlocks(data.blocks, data.blockStates);

            if (version >= generationDataVersion)
            {
                const auto generation = reader.compressed(compressedGeneration, 2 * maskBytes + caveCount * 2 * sizeof(uint32_t));
                data.terrainAirMask.resize(maskWords);
                data.terrainSolidCubeMask.resize(maskWords);
                memcpy(data.terrainAirMask.data(), generation.data(), maskBytes);
                memcpy(data.terrainSolidCubeMask.data(), generation.data() + maskBytes, maskBytes);
                for (size_t word = 0; word < maskWords; ++word)
                {
                    require((data.terrainAirMask[word] & data.terrainSolidCubeMask[word]) == 0, "overlapping terrain masks");
                }
                for (uint32_t c = 0; c < caveCount; ++c)
                {
                    const size_t offset = 2 * maskBytes + c * 2 * sizeof(uint32_t);
                    const uint32_t packed = wordAt(generation, offset);
                    const auto position = candidatePosition(packed, origin, static_cast<uint32_t>(CaveStructureType::COUNT));
                    const uint32_t height = wordAt(generation, offset + sizeof(uint32_t));
                    require(height > 0 && height <= chunkSizeY, "invalid cave height");
                    data.caveStructures.push_back({ static_cast<CaveStructureType>(packed & 0xffu), position,
                                                    static_cast<int>(height) });
                }
            }
            chunks.push_back({ chunkPos, std::move(data) });
        }
        reader.finish();
        return chunks;
    }
    catch (const std::exception& error)
    {
        Logger::logError("region import: %s: %s", path.generic_string().c_str(), error.what());
        return std::nullopt;
    }
}
} // namespace RegionFile
