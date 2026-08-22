// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "biome_noise.h"

#include "util/rng.h"

#include <vector>

#include <FastNoise/FastNoise.h>

using namespace glm;
namespace FN = FastNoise;

namespace BiomeNoiseField
{

static FN::SmartNode<FN::Generator> fnTemperature;
static FN::SmartNode<FN::Generator> fnHumidity;
static FN::SmartNode<FN::Generator> fnPeak;
static FN::SmartNode<FN::Generator> fnInland;
inline constexpr float biomeNoiseScale = 1000.f;

// Shared by fillGrids and sampleAt so single-point samples match the grids
static int fieldSeed;
static ivec2 noiseOffsetXZ;

void init(uint32_t worldSeed)
{
    fieldSeed = static_cast<int>(worldSeed ^ hash(719023919));
    RandomNumberGenerator rng = initRng(worldSeed ^ hash(8810091029));
    noiseOffsetXZ = ivec2(rng.nextInt(-4096, 4096), rng.nextInt(-4096, 4096));

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(5689481209);
        fnSimplex->SetScale(2.5f * biomeNoiseScale);
        fnSimplex->SetOutputMin(-0.7f);
        fnSimplex->SetOutputMax(0.7f);
        auto fnWarp = FN::New<FN::DomainWarpGradient>();
        fnWarp->SetSource(fnSimplex);
        fnWarp->SetScale(0.06f * biomeNoiseScale);
        fnWarp->SetWarpAmplitude(0.02f * biomeNoiseScale);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnWarp);
        fnFractal->SetOctaveCount(3);

        fnTemperature = fnFractal;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(680199230);
        fnSimplex->SetScale(1.5f * biomeNoiseScale);
        fnSimplex->SetOutputMin(-0.7f);
        fnSimplex->SetOutputMax(0.7f);
        auto fnWarp = FN::New<FN::DomainWarpGradient>();
        fnWarp->SetSource(fnSimplex);
        fnWarp->SetScale(0.04f * biomeNoiseScale);
        fnWarp->SetWarpAmplitude(0.03f * biomeNoiseScale);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnWarp);
        fnFractal->SetOctaveCount(3);

        fnHumidity = fnFractal;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(901992021);
        fnSimplex->SetScale(2.5f * biomeNoiseScale);
        fnSimplex->SetOutputMin(0.0f);
        fnSimplex->SetOutputMax(1.0f);
        auto fnFractalRidged = FN::New<FN::FractalRidged>();
        fnFractalRidged->SetSource(fnSimplex);
        fnFractalRidged->SetOctaveCount(5);
        auto fnMultiply = FN::New<FN::Multiply>();
        fnMultiply->SetLHS(fnFractalRidged);
        fnMultiply->SetRHS(0.7f);

        fnPeak = fnMultiply;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(76123912);
        fnSimplex->SetScale(5.f * biomeNoiseScale);
        fnSimplex->SetOutputMin(-1.0f);
        fnSimplex->SetOutputMax(1.0f);
        auto fnWarp = FN::New<FN::DomainWarpGradient>();
        fnWarp->SetSource(fnSimplex);
        fnWarp->SetScale(0.04f * biomeNoiseScale);
        fnWarp->SetWarpAmplitude(0.02f * biomeNoiseScale);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnWarp);
        fnFractal->SetOctaveCount(5);

        fnInland = fnFractal;
    }
}

glm::ivec2 getNoiseOffsetXZ()
{
    return noiseOffsetXZ;
}

void fillGrids(const BiomeNoiseGrids& grids, vec2 startXZ, glm::uvec2 numSamples, float stepBlocks)
{
    const auto fill = [&](float* data, const FN::SmartNode<FN::Generator>& fn)
    {
        fn->GenUniformGrid2D(data,
                             startXZ.x + noiseOffsetXZ.x,
                             startXZ.y + noiseOffsetXZ.y /*z*/,
                             numSamples.x,
                             numSamples.y,
                             stepBlocks,
                             stepBlocks,
                             fieldSeed);
    };
    fill(grids.temperature, fnTemperature);
    fill(grids.humidity, fnHumidity);
    fill(grids.peak, fnPeak);
    fill(grids.inland, fnInland);
}

BiomeNoise sampleAt(vec2 posXZ_WS)
{
    const float x = posXZ_WS.x + noiseOffsetXZ.x;
    const float z = posXZ_WS.y + noiseOffsetXZ.y /*z*/;
    return {
        .temperature = fnTemperature->GenSingle2D(x, z, fieldSeed),
        .humidity = fnHumidity->GenSingle2D(x, z, fieldSeed),
        .peak = fnPeak->GenSingle2D(x, z, fieldSeed),
        .inland = fnInland->GenSingle2D(x, z, fieldSeed),
    };
}

BiomeNoise noiseAt(const BiomeNoiseGrids& grids, uint32_t idx)
{
    return {
        .temperature = grids.temperature[idx],
        .humidity = grids.humidity[idx],
        .peak = grids.peak[idx],
        .inland = grids.inland[idx],
    };
}

float computeFloodFactor(const BiomeNoise& biomeNoise)
{
    // min, not product: the factor is limited by its worst axis, instead of requiring every axis
    // to be near-perfect at once.
    return min(min(smoothstep(-0.1f, 0.35f, biomeNoise.temperature),
                   smoothstep(0.0f, 0.45f, biomeNoise.humidity)),
               min(smoothstep(-0.1f, -0.55f, biomeNoise.peak),
                   min(smoothstep(0.2f, 0.3f, biomeNoise.inland),
                       smoothstep(0.85f, 0.75f, biomeNoise.inland))));
}

Biome biomeFromNoise(const BiomeNoise& biomeNoise)
{
    if (computeFloodFactor(biomeNoise) > floodTintThreshold)
    {
        return Biome::SWAMP;
    }
    return Biomes::getClosestBiome(biomeNoise);
}

void fillBiomeRect(Biome* outBiomes, glm::ivec2 originBlocksXZ_WS, glm::uvec2 numTexels, uint32_t texelSizeBlocks)
{
    const uint32_t numSamples = numTexels.x * numTexels.y;
    std::vector<float> temperatureNoise(numSamples);
    std::vector<float> humidityNoise(numSamples);
    std::vector<float> peakNoise(numSamples);
    std::vector<float> inlandNoise(numSamples);
    const BiomeNoiseGrids grids = {
        .temperature = temperatureNoise.data(),
        .humidity = humidityNoise.data(),
        .peak = peakNoise.data(),
        .inland = inlandNoise.data(),
    };

    const vec2 texelCentersStartXZ = vec2(originBlocksXZ_WS) + texelSizeBlocks * 0.5f;
    fillGrids(grids, texelCentersStartXZ, numTexels, static_cast<float>(texelSizeBlocks));

    for (uint32_t idx = 0; idx < numSamples; ++idx)
    {
        outBiomes[idx] = biomeFromNoise(noiseAt(grids, idx));
    }
}

} // namespace BiomeNoiseField
