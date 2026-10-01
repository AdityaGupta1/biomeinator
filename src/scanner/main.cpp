// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "terrain/biome.h"
#include "terrain/biome_noise.h"

#include <httplib.h>
#include <json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{

// Map display colors, chunkbase-style. Not the in-game grass tints — those are all similar greens
// and would be unreadable as a map palette. One entry per Biome, in enum order.
constexpr std::array<const char*, static_cast<size_t>(Biome::COUNT)> biomeMapColors = {
    "#2e5cb8", // OCEAN
    "#fade55", // BEACH
    "#a7a7a7", // GRAVEL_BEACH
    "#46464b", // BLACK_SAND_BEACH
    "#8db360", // PLAINS
    "#fa9418", // DESERT
    "#056621", // FOREST
    "#dde8ed", // TUNDRA
    "#bdb25f", // SAVANNA
    "#a5d7e3", // ICE_FIELDS
    "#606060", // MOUNTAINS
    "#6a7039", // SWAMP
    "#c97539", // MESA
    "#789479", // TIANZI_MOUNTAINS
    "#df542c", // RED_DESERT
    "#39c99b", // OASIS
};

// BiomeNoiseFields state is global; serialize seed switches and fills across server threads.
std::mutex noiseMutex;
uint32_t currentSeed = 0;
bool seedInitialized = false;

// Caller must hold noiseMutex
void ensureSeed(uint32_t seed)
{
    if (!seedInitialized || seed != currentSeed)
    {
        BiomeNoiseFields::init(seed);
        currentSeed = seed;
        seedInitialized = true;
    }
}

constexpr int64_t maxTexelsPerRequest = 8'000'000;
// fillBiomeRect works in int block coordinates, and its oasis lookup allocates one pond per
// 384-block cell of the covered rect, so the rect itself is bounded, not just the texel count.
constexpr int64_t maxCoordinateBlocks = int64_t(1) << 28;
constexpr int64_t maxCoveredBlocksPerAxis = int64_t(1) << 24;
constexpr int64_t maxCoveredOasisCells = int64_t(1) << 20;

bool isCoveredRectValid(int64_t x0, int64_t z0, int64_t sizeX, int64_t sizeZ)
{
    if (std::abs(x0) > maxCoordinateBlocks || std::abs(z0) > maxCoordinateBlocks ||
        sizeX > maxCoveredBlocksPerAxis || sizeZ > maxCoveredBlocksPerAxis)
    {
        return false;
    }
    constexpr int64_t oasisCellSize = 384;
    return (sizeX / oasisCellSize + 3) * (sizeZ / oasisCellSize + 3) <= maxCoveredOasisCells;
}

bool tryGetIntParam(const httplib::Request& req, const char* name, int64_t& outValue)
{
    if (!req.has_param(name))
    {
        return false;
    }
    try
    {
        outValue = std::stoll(req.get_param_value(name));
    }
    catch (const std::exception&)
    {
        return false;
    }
    return true;
}

void setBadRequest(httplib::Response& res, const char* message)
{
    res.status = 400;
    res.set_content(message, "text/plain");
}

struct CoverageOptions
{
    int64_t seedStart{ 1 };
    int64_t seedCount{ 8 };
    int64_t sizeBlocks{ 32768 };
    int64_t step{ 32 };
    // Patches narrower than this (as the side of a square of equal area) count as slivers
    int64_t sliverWidthBlocks{ 128 };
};

bool parseCoverageOptions(int argc, char** argv, CoverageOptions& outOptions)
{
    const std::array<std::pair<const char*, int64_t*>, 5> optionFields{{
        { "--seedStart=", &outOptions.seedStart },
        { "--seedCount=", &outOptions.seedCount },
        { "--size=", &outOptions.sizeBlocks },
        { "--step=", &outOptions.step },
        { "--sliverWidth=", &outOptions.sliverWidthBlocks },
    }};
    for (int argIdx = 2; argIdx < argc; ++argIdx)
    {
        const std::string arg = argv[argIdx];
        bool matched = false;
        for (const auto& [prefix, field] : optionFields)
        {
            if (arg.rfind(prefix, 0) == 0)
            {
                try
                {
                    *field = std::stoll(arg.substr(std::strlen(prefix)));
                }
                catch (const std::exception&)
                {
                    return false;
                }
                matched = true;
            }
        }
        if (!matched)
        {
            return false;
        }
    }
    return outOptions.seedCount > 0 && outOptions.step > 0 && outOptions.sizeBlocks >= outOptions.step &&
           outOptions.sliverWidthBlocks >= 0;
}

struct BiomeCoverage
{
    int64_t numTexels{ 0 };
    // Areas in texels of patches that don't touch the scanned square's edge, whose true size is unknown
    std::vector<int64_t> patchSizes;
};

// Labels 4-connected same-biome patches, adding each interior patch's size to its biome's list
void collectPatches(const std::vector<Biome>& biomes, int64_t texelsPerSide, std::vector<BiomeCoverage>& coverage)
{
    std::vector<bool> visited(biomes.size(), false);
    std::vector<int64_t> stack;
    for (int64_t startIdx = 0; startIdx < static_cast<int64_t>(biomes.size()); ++startIdx)
    {
        if (visited[startIdx])
        {
            continue;
        }
        const Biome biome = biomes[startIdx];
        int64_t patchSize = 0;
        bool touchesEdge = false;
        visited[startIdx] = true;
        stack.push_back(startIdx);
        while (!stack.empty())
        {
            const int64_t idx = stack.back();
            stack.pop_back();
            ++patchSize;
            const int64_t x = idx % texelsPerSide;
            const int64_t z = idx / texelsPerSide;
            touchesEdge |= x == 0 || z == 0 || x == texelsPerSide - 1 || z == texelsPerSide - 1;
            const std::array<std::pair<int64_t, int64_t>, 4> neighbors{{ { x - 1, z }, { x + 1, z }, { x, z - 1 }, { x, z + 1 } }};
            for (const auto& [nx, nz] : neighbors)
            {
                if (nx < 0 || nz < 0 || nx >= texelsPerSide || nz >= texelsPerSide)
                {
                    continue;
                }
                const int64_t neighborIdx = nx + nz * texelsPerSide;
                if (!visited[neighborIdx] && biomes[neighborIdx] == biome)
                {
                    visited[neighborIdx] = true;
                    stack.push_back(neighborIdx);
                }
            }
        }
        if (!touchesEdge)
        {
            coverage[static_cast<size_t>(biome)].patchSizes.push_back(patchSize);
        }
    }
}

// Prints each biome's share of land and its patch-size distribution over a square centered on the
// origin, accumulated across a range of seeds
int runCoverage(const CoverageOptions& options)
{
    const int64_t texelsPerSide = options.sizeBlocks / options.step;
    const int64_t halfSizeBlocks = texelsPerSide * options.step / 2;
    if (texelsPerSide * texelsPerSide > maxTexelsPerRequest ||
        !isCoveredRectValid(-halfSizeBlocks, -halfSizeBlocks, 2 * halfSizeBlocks, 2 * halfSizeBlocks))
    {
        fprintf(stderr, "coverage: scanned square too large; raise --step or lower --size\n");
        return 1;
    }

    std::vector<BiomeCoverage> coverage(static_cast<size_t>(Biome::COUNT));
    std::vector<Biome> biomes(texelsPerSide * texelsPerSide);
    for (int64_t seed = options.seedStart; seed < options.seedStart + options.seedCount; ++seed)
    {
        BiomeNoiseFields::init(static_cast<uint32_t>(seed));
        BiomeNoiseFields::fillBiomeRect(biomes.data(),
                                       glm::ivec2(-halfSizeBlocks),
                                       glm::uvec2(texelsPerSide),
                                       static_cast<uint32_t>(options.step));
        for (const Biome biome : biomes)
        {
            ++coverage[static_cast<size_t>(biome)].numTexels;
        }
        collectPatches(biomes, texelsPerSide, coverage);
    }

    int64_t numLandTexels = 0;
    for (size_t idx = 0; idx < coverage.size(); ++idx)
    {
        if (Biomes::getBiomeData(static_cast<Biome>(idx)).tier != BiomeTier::OCEAN)
        {
            numLandTexels += coverage[idx].numTexels;
        }
    }

    std::vector<size_t> order(coverage.size());
    for (size_t idx = 0; idx < order.size(); ++idx)
    {
        order[idx] = idx;
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return coverage[a].numTexels > coverage[b].numTexels; });

    const double texelArea = static_cast<double>(options.step * options.step);
    const double sliverArea = static_cast<double>(options.sliverWidthBlocks * options.sliverWidthBlocks);
    const auto widthBlocks = [&](double areaTexels) { return std::sqrt(areaTexels * texelArea); };

    printf("Seeds %lld-%lld, %lld x %lld blocks at %lld-block steps. Widths are sqrt(patch area) over patches\n"
           "not touching the square's edge; slivers are patches narrower than %lld blocks.\n\n",
           options.seedStart, options.seedStart + options.seedCount - 1, 2 * halfSizeBlocks, 2 * halfSizeBlocks,
           options.step, options.sliverWidthBlocks);
    printf("%-20s %7s %8s %10s %12s %8s %10s\n", "biome", "land %", "patches", "median w", "area-wtd w", "slivers",
           "sliver %");
    for (const size_t idx : order)
    {
        BiomeCoverage& biomeCoverage = coverage[idx];
        if (biomeCoverage.numTexels == 0)
        {
            continue;
        }
        const Biome biome = static_cast<Biome>(idx);
        const bool isOcean = Biomes::getBiomeData(biome).tier == BiomeTier::OCEAN;
        std::vector<int64_t>& sizes = biomeCoverage.patchSizes;
        std::sort(sizes.begin(), sizes.end());

        double sumArea = 0.0;
        double sumSquaredArea = 0.0;
        int64_t numSlivers = 0;
        double sliverTexels = 0.0;
        for (const int64_t size : sizes)
        {
            sumArea += size;
            sumSquaredArea += static_cast<double>(size) * size;
            if (size * texelArea < sliverArea)
            {
                ++numSlivers;
                sliverTexels += size;
            }
        }
        const double medianWidth = sizes.empty() ? 0.0 : widthBlocks(static_cast<double>(sizes[sizes.size() / 2]));
        // Width of the patch a random point of this biome lies in, on average
        const double areaWeightedWidth = sumArea > 0.0 ? widthBlocks(sumSquaredArea / sumArea) : 0.0;

        char landShare[16];
        if (isOcean)
        {
            snprintf(landShare, sizeof(landShare), "-");
        }
        else
        {
            snprintf(landShare, sizeof(landShare), "%.1f", 100.0 * biomeCoverage.numTexels / numLandTexels);
        }
        printf("%-20s %7s %8zu %10.0f %12.0f %8lld %10.1f\n", Biomes::getBiomeData(biome).name, landShare, sizes.size(),
               medianWidth, areaWeightedWidth, numSlivers, sumArea > 0.0 ? 100.0 * sliverTexels / sumArea : 0.0);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Biomes::init();

    if (argc > 1 && std::strcmp(argv[1], "--coverage") == 0)
    {
        CoverageOptions options;
        if (!parseCoverageOptions(argc, argv, options))
        {
            fprintf(stderr, "usage: BiomeScanner --coverage [--seedStart=N] [--seedCount=N] [--size=blocks] "
                            "[--step=blocks] [--sliverWidth=blocks]\n");
            return 1;
        }
        return runCoverage(options);
    }

    const int port = (argc > 1) ? std::atoi(argv[1]) : 8080;

    httplib::Server server;

    // Served from the source tree so the page can be edited and refreshed without rebuilding
    server.Get("/", [](const httplib::Request&, httplib::Response& res)
    {
        std::ifstream file(std::string(CMAKE_SOURCE_DIR) + "/src/scanner/index.html", std::ios::binary);
        if (!file)
        {
            res.status = 500;
            res.set_content("index.html not found", "text/plain");
            return;
        }
        std::stringstream contents;
        contents << file.rdbuf();
        res.set_content(contents.str(), "text/html");
    });

    server.Get("/api/biomeInfo", [](const httplib::Request&, httplib::Response& res)
    {
        nlohmann::json out = nlohmann::json::array();
        for (size_t idx = 0; idx < static_cast<size_t>(Biome::COUNT); ++idx)
        {
            out.push_back({
                { "id", idx },
                { "name", Biomes::getBiomeData(static_cast<Biome>(idx)).name },
                { "color", biomeMapColors[idx] },
            });
        }
        res.set_content(out.dump(), "application/json");
    });

    // Returns one byte per texel (the Biome enum value), x-innermost, row-major from (x0, z0)
    server.Get("/api/biomes", [](const httplib::Request& req, httplib::Response& res)
    {
        int64_t seed, x0, z0, numTexelsX, numTexelsZ, texelSizeBlocks;
        if (!tryGetIntParam(req, "seed", seed) || !tryGetIntParam(req, "x0", x0) ||
            !tryGetIntParam(req, "z0", z0) || !tryGetIntParam(req, "w", numTexelsX) ||
            !tryGetIntParam(req, "h", numTexelsZ) || !tryGetIntParam(req, "step", texelSizeBlocks))
        {
            setBadRequest(res, "required params: seed, x0, z0, w, h, step");
            return;
        }
        // Each axis is capped before multiplying so the product can't overflow
        if (numTexelsX <= 0 || numTexelsZ <= 0 || numTexelsX > maxTexelsPerRequest ||
            numTexelsZ > maxTexelsPerRequest || numTexelsX * numTexelsZ > maxTexelsPerRequest || texelSizeBlocks <= 0 ||
            texelSizeBlocks > maxCoveredBlocksPerAxis ||
            !isCoveredRectValid(x0, z0, numTexelsX * texelSizeBlocks, numTexelsZ * texelSizeBlocks))
        {
            setBadRequest(res, "invalid dimensions");
            return;
        }

        std::vector<Biome> biomes(numTexelsX * numTexelsZ);
        {
            std::scoped_lock<std::mutex> lock(noiseMutex);
            ensureSeed(static_cast<uint32_t>(seed));
            BiomeNoiseFields::fillBiomeRect(biomes.data(),
                                           glm::ivec2(x0, z0),
                                           glm::uvec2(numTexelsX, numTexelsZ),
                                           static_cast<uint32_t>(texelSizeBlocks));
        }

        res.set_content(reinterpret_cast<const char*>(biomes.data()), biomes.size(), "application/octet-stream");
    });

    // Scans [seedStart, seedStart + seedCount) and reports, per seed, the fraction of texels
    // matching the target biome within a square of +-radius blocks around the origin
    server.Get("/api/search", [](const httplib::Request& req, httplib::Response& res)
    {
        int64_t biomeId, radiusBlocks, seedStart, seedCount, texelSizeBlocks;
        if (!tryGetIntParam(req, "biome", biomeId) || !tryGetIntParam(req, "radius", radiusBlocks) ||
            !tryGetIntParam(req, "seedStart", seedStart) || !tryGetIntParam(req, "seedCount", seedCount) ||
            !tryGetIntParam(req, "step", texelSizeBlocks))
        {
            setBadRequest(res, "required params: biome, radius, seedStart, seedCount, step");
            return;
        }
        // The radius cap also keeps the arithmetic below far from overflow
        if (biomeId < 0 || biomeId >= static_cast<int64_t>(Biome::COUNT) || radiusBlocks <= 0 ||
            radiusBlocks > maxTexelsPerRequest || seedCount <= 0 || seedCount > 1000 || texelSizeBlocks <= 0 ||
            !isCoveredRectValid(-radiusBlocks, -radiusBlocks, 2 * radiusBlocks, 2 * radiusBlocks))
        {
            setBadRequest(res, "invalid params");
            return;
        }
        const int64_t texelsPerSide = 2 * radiusBlocks / texelSizeBlocks;
        if (texelsPerSide <= 0 || texelsPerSide * texelsPerSide > maxTexelsPerRequest)
        {
            setBadRequest(res, "invalid radius/step");
            return;
        }

        const Biome targetBiome = static_cast<Biome>(biomeId);
        std::vector<Biome> biomes(texelsPerSide * texelsPerSide);
        nlohmann::json out = nlohmann::json::array();
        {
            std::scoped_lock<std::mutex> lock(noiseMutex);
            for (int64_t seed = seedStart; seed < seedStart + seedCount; ++seed)
            {
                ensureSeed(static_cast<uint32_t>(seed));
                BiomeNoiseFields::fillBiomeRect(biomes.data(),
                                               glm::ivec2(-radiusBlocks),
                                               glm::uvec2(texelsPerSide),
                                               static_cast<uint32_t>(texelSizeBlocks));

                size_t matchCount = 0;
                for (const Biome biome : biomes)
                {
                    matchCount += (biome == targetBiome);
                }
                out.push_back({
                    { "seed", seed },
                    { "fraction", static_cast<double>(matchCount) / static_cast<double>(biomes.size()) },
                });
            }
        }

        res.set_content(out.dump(), "application/json");
    });

    printf("BiomeScanner listening on http://127.0.0.1:%d\n", port);
    if (!server.listen("127.0.0.1", port))
    {
        fprintf(stderr, "failed to listen on port %d\n", port);
        return 1;
    }
    return 0;
}
