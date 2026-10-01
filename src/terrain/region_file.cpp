// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "region_file.h"

#include "logger.h"
#include "util/file_util.h"

#include <lz4.h>

#include <algorithm>
#include <bitset>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>

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
constexpr size_t heightBytes = chunkSizeXZSquare * sizeof(uint16_t);
constexpr size_t candidatesOffset = 2 * maskBytes + 2 * heightBytes;
// v5/v6 can include accepted exposed-surface trees as well as grid structures, so this is far above
// maxGridStructuresPerChunk.
constexpr uint32_t maxStructures = numChunkBlocks / 2;
constexpr size_t structureBytesLimit = sizeof(uint32_t) * (1 + maxStructures);
constexpr uint32_t maxSurfaceCandidates = numChunkBlocks;
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
// v6 adds block-state count(u32). v7 adds generation LZ4 size, cave count, and
// surface-placement count (all u32).
// Payload order: LZ4(blocks + biomes), LZ4(grid/legacy structures), packed block states, then v7
// LZ4(air mask + solid-cube mask + terrain top Y (u16/column) + terrain surface height (u16/column)
// + ordered cave and surface-placement candidates). Legacy imports have no heights and re-export
// zeros, which neighbors treat as missing.
// The structure payload starts with its count(u32). Each structure's type/position
// is packed [type:8, x:4, y:9, z:4, unused:7]; cave records append availableHeight(u32).
// Surface-placement candidates append position (type=0), stable generator ID, priority,
// headroom (all u32). Rejected candidates also affect neighboring placement decisions.

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool isSurfaceMount(const Registry& registry, Block block)
{
    const size_t index = static_cast<size_t>(block);
    require(index < registry.blockStateKinds.size(), "invalid block");
    return registry.blockStateKinds[index] == BlockStateKind::SURFACE_MOUNT;
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

void validateBlocks(std::span<const Block> blocks, const std::unordered_map<uint32_t, uint8_t>& states,
                    const Registry& registry)
{
    require(states.size() <= numChunkBlocks, "too many block states");
    for (const auto& [index, state] : states)
    {
        require(index < blocks.size() && state < blockFaceCount && isSurfaceMount(registry, blocks[index]),
                "invalid block state");
    }
    for (uint32_t index = 0; index < blocks.size(); ++index)
    {
        require(!isSurfaceMount(registry, blocks[index]) || states.contains(index), "missing surface-mount block state");
    }
}

template<class T>
void appendSpan(std::vector<char>& bytes, std::span<const T> values)
{
    append(bytes, values.data(), values.size_bytes());
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

bool write(const std::filesystem::path& path, glm::ivec2 position, std::span<const SerializedChunkView> inputChunks,
           const Registry& registry)
{
    try
    {
        require(isValidPosition(position), "region coordinates out of range");
        require(inputChunks.size() <= regionChunkCount, "too many chunks");
        const glm::ivec2 regionOrigin = position * static_cast<int>(regionSideLength);
        std::vector<std::pair<uint16_t, const SerializedChunkView*>> chunks;
        chunks.reserve(inputChunks.size());
        std::bitset<regionChunkCount> seen;
        for (const SerializedChunkView& chunk : inputChunks)
        {
            const int64_t localX = static_cast<int64_t>(chunk.position.x) - regionOrigin.x;
            const int64_t localZ = static_cast<int64_t>(chunk.position.y) - regionOrigin.y;
            require(localX >= 0 && localX < regionSideLength && localZ >= 0 && localZ < regionSideLength,
                    "chunk outside region");
            const uint16_t index = static_cast<uint16_t>(localX + localZ * regionSideLength);
            require(!seen[index], "duplicate chunk");
            seen.set(index);
            chunks.emplace_back(index, &chunk);
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
            for (const auto& [index, chunkPtr] : chunks)
            {
                const SerializedChunkView& chunk = *chunkPtr;
                // Reuse scratch for one encoded chunk; never retain a whole region's output.
                bytes.clear();
                require(chunk.blocks.size() == numChunkBlocks && chunk.biomes.size() == chunkSizeXZSquare &&
                        chunk.blockStates && chunk.terrainAirMask.size() == maskWords &&
                        chunk.terrainSolidCubeMask.size() == maskWords, "incomplete chunk data");
                require((chunk.terrainTopY.empty() && chunk.terrainSurfaceHeight.empty()) ||
                        (chunk.terrainTopY.size() == chunkSizeXZSquare &&
                         chunk.terrainSurfaceHeight.size() == chunkSizeXZSquare), "incomplete terrain heights");
                for (Biome biome : chunk.biomes)
                {
                    require(biome < Biome::COUNT, "invalid biome");
                }
                for (size_t word = 0; word < maskWords; ++word)
                {
                    require((chunk.terrainAirMask[word] & chunk.terrainSolidCubeMask[word]) == 0,
                            "overlapping terrain masks");
                }
                for (uint16_t topY : chunk.terrainTopY)
                {
                    require(topY < chunkSizeY, "invalid terrain top");
                }
                validateBlocks(chunk.blocks, *chunk.blockStates, registry);
                const glm::ivec2 origin = chunk.position * static_cast<int>(chunkSizeXZ);

                append(bytes, index);
                const size_t headerOffset = bytes.size();
                bytes.resize(headerOffset + 6 * sizeof(uint32_t));
                payload.clear();
                appendSpan(payload, chunk.blocks);
                appendSpan(payload, chunk.biomes);
                const uint32_t compressedBlocks = compress(bytes, payload);

                uint32_t compressedStructures = 0;
                require(chunk.structures.size() <= maxStructures, "too many structures");
                if (!chunk.structures.empty())
                {
                    payload.clear();
                    append(payload, static_cast<uint32_t>(chunk.structures.size()));
                    for (const Structure& structure : chunk.structures)
                    {
                        append(payload, packCandidate(static_cast<uint32_t>(structure.type), structure.pos_WS, origin,
                                                      static_cast<uint32_t>(StructureType::COUNT)));
                    }
                    compressedStructures = compress(bytes, payload);
                }

                std::vector<std::pair<uint32_t, uint8_t>> sortedStates(chunk.blockStates->begin(),
                                                                       chunk.blockStates->end());
                std::sort(sortedStates.begin(), sortedStates.end());
                for (const auto& [blockIndex, state] : sortedStates)
                {
                    append(bytes, blockIndex | (static_cast<uint32_t>(state) << blockStateIndexBits));
                }

                payload.clear();
                appendSpan(payload, chunk.terrainAirMask);
                appendSpan(payload, chunk.terrainSolidCubeMask);
                if (chunk.terrainTopY.empty())
                {
                    payload.resize(payload.size() + 2 * heightBytes);
                }
                else
                {
                    appendSpan(payload, chunk.terrainTopY);
                    appendSpan(payload, chunk.terrainSurfaceHeight);
                }
                require(chunk.caveStructures.size() <= maxCaveStructures, "too many cave candidates");
                for (const CaveStructure& cave : chunk.caveStructures)
                {
                    append(payload, packCandidate(static_cast<uint32_t>(cave.type), cave.pos_WS, origin,
                                                  static_cast<uint32_t>(CaveStructureType::COUNT)));
                    require(cave.availableHeight > 0 && cave.availableHeight <= chunkSizeY, "invalid cave height");
                    append(payload, static_cast<uint32_t>(cave.availableHeight));
                }
                const auto& surfaceCandidates = chunk.surfaceStructureCandidates;
                require(surfaceCandidates.size() <= maxSurfaceCandidates, "too many surface-placement candidates");
                for (const SurfaceStructureCandidate& candidate : surfaceCandidates)
                {
                    append(payload, packCandidate(0, candidate.pos_WS, origin, 1));
                    require(candidate.gen && candidate.gen->surfacePlacement, "candidate without surface generator");
                    const SurfaceStructureGenId id = candidate.gen->surfacePlacement->id;
                    const auto registered = registry.surfaceStructureGens->find(id);
                    require(registered != registry.surfaceStructureGens->end() && registered->second == candidate.gen,
                            "unregistered surface generator");
                    require(candidate.headroom > 0 && candidate.headroom <= chunkSizeY - candidate.pos_WS.y,
                            "invalid surface headroom");
                    append(payload, static_cast<uint32_t>(id));
                    append(payload, candidate.priority);
                    append(payload, candidate.headroom);
                }
                const uint32_t compressedGeneration = compress(bytes, payload);
                const uint32_t header[] = {
                    compressedBlocks,
                    compressedStructures,
                    static_cast<uint32_t>(sortedStates.size()),
                    compressedGeneration,
                    static_cast<uint32_t>(chunk.caveStructures.size()),
                    static_cast<uint32_t>(surfaceCandidates.size()),
                };
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
                                  std::span<const Block> blockRemap, const Registry& registry)
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
        const bool hasGenerationData = version >= generationDataVersion;
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
            const uint32_t compressedGeneration = hasGenerationData ? reader.value<uint32_t>() : 0;
            const uint32_t caveCount = hasGenerationData ? reader.value<uint32_t>() : 0;
            const uint32_t surfaceCount = hasGenerationData ? reader.value<uint32_t>() : 0;
            require(stateCount <= numChunkBlocks && caveCount <= maxCaveStructures &&
                    surfaceCount <= maxSurfaceCandidates, "invalid chunk payload count");

            SerializedChunkData data;
            const auto payload = reader.compressed(compressedBlocks, blockBiomeBytes);
            data.blocks.resize(numChunkBlocks);
            data.biomes.resize(chunkSizeXZSquare);
            memcpy(data.blocks.data(), payload.data(), numChunkBlocks * sizeof(Block));
            memcpy(data.biomes.data(), payload.data() + numChunkBlocks * sizeof(Block),
                   chunkSizeXZSquare * sizeof(Biome));
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
                require(structureCount <= maxStructures &&
                        candidates.size() == sizeof(uint32_t) * (1 + structureCount),
                        "invalid structure count");
                for (uint32_t s = 0; s < structureCount; ++s)
                {
                    const uint32_t packed = wordAt(candidates, sizeof(uint32_t) * (1 + s));
                    const auto position =
                        candidatePosition(packed, origin, static_cast<uint32_t>(StructureType::COUNT));
                    data.structures.push_back({ static_cast<StructureType>(packed & 0xffu), position });
                }
            }
            for (uint32_t s = 0; s < stateCount; ++s)
            {
                const uint32_t packed = reader.value<uint32_t>();
                const uint32_t blockIndex = packed & blockStateIndexMask;
                const uint32_t state = packed >> blockStateIndexBits;
                require(state < blockFaceCount, "invalid packed block state");
                require(data.blockStates.emplace(blockIndex, static_cast<uint8_t>(state)).second,
                        "duplicate block state");
            }
            if (version == oldestVersion)
            {
                for (uint32_t blockIndex = 0; blockIndex < numChunkBlocks; ++blockIndex)
                {
                    if (isSurfaceMount(registry, data.blocks[blockIndex]))
                    {
                        data.blockStates.emplace(blockIndex, blockFaceIndex(BlockFace::Y_POS));
                    }
                }
            }
            validateBlocks(data.blocks, data.blockStates, registry);

            if (hasGenerationData)
            {
                const size_t surfaceOffset = candidatesOffset + caveCount * 2 * sizeof(uint32_t);
                const size_t generationBytes = surfaceOffset + surfaceCount * 4 * sizeof(uint32_t);
                const auto generation = reader.compressed(compressedGeneration, generationBytes);
                data.terrainAirMask.resize(maskWords);
                data.terrainSolidCubeMask.resize(maskWords);
                data.terrainTopY.resize(chunkSizeXZSquare);
                data.terrainSurfaceHeight.resize(chunkSizeXZSquare);
                memcpy(data.terrainAirMask.data(), generation.data(), maskBytes);
                memcpy(data.terrainSolidCubeMask.data(), generation.data() + maskBytes, maskBytes);
                memcpy(data.terrainTopY.data(), generation.data() + 2 * maskBytes, heightBytes);
                memcpy(data.terrainSurfaceHeight.data(), generation.data() + 2 * maskBytes + heightBytes, heightBytes);
                for (size_t word = 0; word < maskWords; ++word)
                {
                    require((data.terrainAirMask[word] & data.terrainSolidCubeMask[word]) == 0,
                            "overlapping terrain masks");
                }
                for (uint16_t topY : data.terrainTopY)
                {
                    require(topY < chunkSizeY, "invalid terrain top");
                }
                for (uint32_t c = 0; c < caveCount; ++c)
                {
                    const size_t offset = candidatesOffset + c * 2 * sizeof(uint32_t);
                    const uint32_t packed = wordAt(generation, offset);
                    const auto position =
                        candidatePosition(packed, origin, static_cast<uint32_t>(CaveStructureType::COUNT));
                    const uint32_t height = wordAt(generation, offset + sizeof(uint32_t));
                    require(height > 0 && height <= chunkSizeY, "invalid cave height");
                    data.caveStructures.push_back({ static_cast<CaveStructureType>(packed & 0xffu), position,
                                                    static_cast<int>(height) });
                }
                for (uint32_t s = 0; s < surfaceCount; ++s)
                {
                    const size_t offset = surfaceOffset + s * 4 * sizeof(uint32_t);
                    const auto position = candidatePosition(wordAt(generation, offset), origin, 1);
                    const auto genId =
                        static_cast<SurfaceStructureGenId>(wordAt(generation, offset + sizeof(uint32_t)));
                    const auto gen = registry.surfaceStructureGens->find(genId);
                    require(gen != registry.surfaceStructureGens->end(), "unknown surface generator ID");
                    const uint32_t priority = wordAt(generation, offset + 2 * sizeof(uint32_t));
                    const uint32_t headroom = wordAt(generation, offset + 3 * sizeof(uint32_t));
                    require(headroom > 0 && headroom <= chunkSizeY - position.y, "invalid surface headroom");
                    data.surfaceStructureCandidates.push_back({ position, gen->second, priority, headroom });
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
