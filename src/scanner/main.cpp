// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "terrain/biome.h"
#include "terrain/biome_noise.h"

#include <httplib.h>
#include <json.hpp>

#include <array>
#include <cstdio>
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

} // namespace

int main(int argc, char** argv)
{
    const int port = (argc > 1) ? std::atoi(argv[1]) : 8080;

    Biomes::init();

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
            numTexelsZ > maxTexelsPerRequest || numTexelsX * numTexelsZ > maxTexelsPerRequest || texelSizeBlocks <= 0)
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
            radiusBlocks > maxTexelsPerRequest || seedCount <= 0 || seedCount > 1000 || texelSizeBlocks <= 0)
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
