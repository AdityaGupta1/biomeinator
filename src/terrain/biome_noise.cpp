// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "biome_noise.h"
#include "oasis_shaping.h"
#include "terrain_formation.h"

#include "rendering/common/common_settings.h"
#include "util/rng.h"

#include <vector>

#include <FastNoise/FastNoise.h>

using namespace glm;
namespace FN = FastNoise;

namespace BiomeNoiseFields
{

static FN::SmartNode<FN::Generator> fnTemperature;
static FN::SmartNode<FN::Generator> fnHumidity;
static FN::SmartNode<FN::Generator> fnPeak;
static FN::SmartNode<FN::Generator> fnInland;
static FN::SmartNode<FN::Generator> fnErosion;
inline constexpr float biomeNoiseScale = 1000.f;

// Shared by fillGrids and sampleAt so single-point samples match the grids
static int noiseFieldSeed;
static ivec2 noiseOffsetXZ;

void init(uint32_t worldSeed)
{
    noiseFieldSeed = static_cast<int>(worldSeed ^ hash(719023919));
    RandomNumberGenerator rng = initRng(worldSeed ^ hash(8810091029));
    noiseOffsetXZ = ivec2(rng.nextInt(-4096, 4096), rng.nextInt(-4096, 4096));

    {
        auto source = FN::New<FN::Simplex>();
        source->SetSeedOffset(186729341);
        source->SetScale(1500.f);
        auto fractal = FN::New<FN::FractalFBm>();
        fractal->SetSource(source);
        fractal->SetOctaveCount(3);
        fnErosion = fractal;
    }
    OasisShaping::init(worldSeed);

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
        if (!data) return;
        fn->GenUniformGrid2D(data,
                             startXZ.x + noiseOffsetXZ.x,
                             startXZ.y + noiseOffsetXZ.y /*z*/,
                             numSamples.x,
                             numSamples.y,
                             stepBlocks,
                             stepBlocks,
                             noiseFieldSeed);
    };
    fill(grids.temperature, fnTemperature);
    fill(grids.humidity, fnHumidity);
    fill(grids.peak, fnPeak);
    fill(grids.inland, fnInland);
    fill(grids.erosion, fnErosion);
}

void fillPositions(const BiomeNoiseGrids& grids, const float* xPositions, const float* zPositions, uint32_t numSamples)
{
    const auto fill = [&](float* data, const FN::SmartNode<FN::Generator>& fn)
    {
        if (data != nullptr)
        {
            fn->GenPositionArray2D(data, numSamples, xPositions, zPositions,
                                  noiseOffsetXZ.x, noiseOffsetXZ.y, noiseFieldSeed);
        }
    };
    fill(grids.temperature, fnTemperature);
    fill(grids.humidity, fnHumidity);
    fill(grids.peak, fnPeak);
    fill(grids.inland, fnInland);
    fill(grids.erosion, fnErosion);
}

BiomeNoise sampleAt(vec2 posXZ_WS)
{
    const float x = posXZ_WS.x + noiseOffsetXZ.x;
    const float z = posXZ_WS.y + noiseOffsetXZ.y /*z*/;
    return {
        .temperature = fnTemperature->GenSingle2D(x, z, noiseFieldSeed),
        .humidity = fnHumidity->GenSingle2D(x, z, noiseFieldSeed),
        .peak = fnPeak->GenSingle2D(x, z, noiseFieldSeed),
        .inland = fnInland->GenSingle2D(x, z, noiseFieldSeed),
        .erosion = fnErosion->GenSingle2D(x, z, noiseFieldSeed),
    };
}

BiomeNoise noiseAt(const BiomeNoiseGrids& grids, uint32_t idx)
{
    return {
        .temperature = grids.temperature[idx],
        .humidity = grids.humidity[idx],
        .peak = grids.peak[idx],
        .inland = grids.inland[idx],
        .erosion = grids.erosion ? grids.erosion[idx] : 0.f,
    };
}

float terraceWeight(const BiomeNoise& n)
{
    return smoothstep(-0.18f, 0.02f, n.erosion) * (1.f - smoothstep(0.27f, 0.48f, n.erosion)) *
           smoothstep(0.1f, 0.3f, n.inland);
}

float dryClimateWeight(const BiomeNoise& n)
{
    return smoothstep(0.12f, 0.38f, n.temperature) * (1.f - smoothstep(-0.25f, 0.02f, n.humidity));
}

float tianziSuitability(const BiomeNoise& n)
{
    // Karst occupies humid, temperate-to-warm rugged regions. Cold or dry mountain
    // climates retain ordinary peaks instead of being intercepted by erosion alone.
    const float temperate = smoothstep(-0.55f, -0.05f, n.temperature) *
                            (1.f - smoothstep(0.65f, 1.05f, n.temperature));
    const float humid = smoothstep(-0.1f, 0.3f, n.humidity);
    const float preserved = 1.f - smoothstep(-0.65f, -0.1f, n.erosion);
    return temperate * humid * preserved * smoothstep(0.08f, 0.32f, n.inland);
}

float tianziWeight(const BiomeNoise& n)
{
    // Ramping the complete suitability (rather than separately fading each axis)
    // keeps the coastal and climate boundaries from cutting through full-height towers.
    return smoothstep(tianziBiomeThreshold, 0.85f, tianziSuitability(n));
}

float surfaceDetailWeight(const BiomeNoise& n)
{
    const float dry = dryClimateWeight(n);
    const float mesa = smoothstep(0.4f, 0.75f, terraceWeight(n)) * smoothstep(0.35f, 0.65f, dry);
    return max(mesa, tianziWeight(n));
}

static float terraceHeight(float height, const BiomeNoise& n)
{
    // Irregular elevation intervals avoid repeating identical shelves up the hillside.
    // Shared anchors keep the remap continuous when either the interval or biome changes.
    constexpr float spacing = 42.f;
    const float offset = 10.f * n.humidity + 6.f * n.temperature;
    const float localHeight = height - offset;
    int band = static_cast<int>(floor((localHeight - SEA_LEVEL) / spacing));
    const auto anchor = [](int index)
    {
        RandomNumberGenerator rng = initRng(noiseFieldSeed ^ 0x7E22ACEu, index);
        return SEA_LEVEL + spacing * (index + rng.nextFloat(-0.32f, 0.32f));
    };
    if (anchor(band) > localHeight) --band;
    else if (anchor(band + 1) < localHeight) ++band;
    const float low = anchor(band), high = anchor(band + 1);
    const float t = clamp((localHeight - low) / (high - low), 0.f, 1.f);
    RandomNumberGenerator rng = initRng(noiseFieldSeed ^ 0x51E1Fu, band);
    const float rampStart = rng.nextFloat(0.15f, 0.4f);
    const float rampEnd = rng.nextFloat(0.75f, 0.95f);
    // Retain a slope across the shelf instead of flattening each tread completely.
    return offset + mix(low, high, mix(t, smoothstep(rampStart, rampEnd, t), 0.8f));
}

NaturalTerrain computeNaturalTerrain(const BiomeNoise& n, vec2 posXZ_WS)
{
    const float peak = clamp((n.peak + 1.f) * 0.5f, 0.f, 1.f);
    const float land = smoothstep(0.f, 0.35f, n.inland);
    const float rugged = 1.f - smoothstep(-0.1f, 0.5f, n.erosion);
    const float dry = dryClimateWeight(n);
    const float terraces = terraceWeight(n) * dry;
    const float tianzi = tianziWeight(n);
    const vec2 pos = posXZ_WS + vec2(noiseOffsetXZ);

    const float inlandHeight = 1.f / (1.f + expf(-10.f * n.inland + 0.1f)) + 0.03f * n.inland - 0.7f;
    const float foundation = 140.f + inlandHeight * 90.f;
    // The modest shared ground still connects all profiles. Restore strong peaks only
    // in preserved highlands, without lifting dry plateaus or roughening flat lowlands.
    const float mountainRelief = mix(8.f, 75.f, rugged) * pow(peak, 2.5f) * (1.f - dry * 0.75f);
    const float mountainClimate = 1.f - smoothstep(0.05f, 0.35f, dry);
    const float highland = rugged * smoothstep(0.25f, 1.05f, n.inland) * mountainClimate;
    const float peakRelief = 160.f * highland * pow(peak, 4.f);
    // Complementary weights blend complete ordinary/Tianzi profiles: as the additional
    // mountain relief recedes, the same weight supplies the stacked formations below.
    float height = foundation + land * (mountainRelief + (1.f - tianzi) * peakRelief);
    if (terraces > 0.f)
    {
        // Blend complete profiles with complementary weights. The same weight replaces
        // ordinary relief and introduces the plateau, so neither can disappear first.
        float mesaHeight = foundation + land * (6.f + 42.f * TerrainFormations::plateauRelief(pos, noiseFieldSeed ^ 0xBA01u));
        mesaHeight = mix(mesaHeight, terraceHeight(mesaHeight, n), 0.45f);
        height = mix(height, mesaHeight, terraces);
    }
    const float coastPull = smoothstep(0.2f, 0.f, abs(n.inland)) * 0.9f;
    height = mix(height, static_cast<float>(SEA_LEVEL + 8), coastPull);
    const float formationBase = height;

    float uplift = 0.f;
    ivec2 formationSite{};
    if (tianzi > 0.f)
    {
        // Split the previous 112-block core budget across three independently sited
        // tiers. Broad summits and narrow rises leave plantable shelves between crowns.
        constexpr std::array<TerrainFormations::Profile, 3> tiers{{
            { 92.f, 34.f, 52.f, 42.f, 8.f, 0.84f, 0.85f },
            { 54.f, 24.f, 30.f, 38.f, 4.f, 0.80f, 0.85f },
            { 37.f, 16.2f, 19.f, 32.f, 2.f, 0.78f, 0.85f },
        }};
        uplift = tianzi * TerrainFormations::sampleStacked(pos, noiseFieldSeed ^ 0x75423u, tiers, &formationSite);
    }
    // Quartz is an explicit formation in dry, non-terraced terrain. It reuses the same
    // finite-support sampler with a narrow summit and a broad foot, not a new noise field.
    const float quartzWeight = dry * land * (1.f - smoothstep(0.f, 0.4f, terraceWeight(n))) * rugged;
    float quartzRadiusFraction = 1.f;
    if (quartzWeight > 0.f)
    {
        constexpr TerrainFormations::Profile spires{ 116.f, 6.5f, 34.f, 42.f, 24.f, 0.04f, 1.f };
        uplift += quartzWeight * TerrainFormations::sample(pos, noiseFieldSeed ^ 0x91337u, spires,
                                                          nullptr, &quartzRadiusFraction);
    }
    height += uplift;

    float amplitude = mix(38.f, 12.f, smoothstep(-0.1f, 0.5f, n.erosion));
    amplitude += 40.f * highland * smoothstep(0.1f, 0.65f, n.peak);
    amplitude = mix(amplitude, 10.f, terraces);
    amplitude = mix(amplitude, 7.f, tianzi);
    amplitude = mix(amplitude, 12.f, dry * (1.f - terraces));
    amplitude = mix(amplitude, 4.f, smoothstep(5.f, 30.f, uplift));
    amplitude /= 1.f + 3.f * smoothstep(0.4f, -0.1f, abs(n.inland));
    return { height, 1.f / amplitude, formationBase, uplift, formationSite, quartzRadiusFraction };
}

float computeFloodFactor(const BiomeNoise& biomeNoise)
{
    const float temperatureFactor = smoothstep(-0.1f, 0.35f, biomeNoise.temperature);
    const float humidityFactor = smoothstep(0.0f, 0.45f, biomeNoise.humidity);
    const float flatFactor = min(smoothstep(-0.1f, -0.55f, biomeNoise.peak),
                                smoothstep(0.25f, 0.55f, biomeNoise.erosion));
    const float inlandFactor =
        min(smoothstep(0.2f, 0.3f, biomeNoise.inland), smoothstep(0.85f, 0.75f, biomeNoise.inland));

    // min, not product: the factor is limited by its worst axis, instead of requiring every axis
    // to be near-perfect at once.
    return min(min(temperatureFactor, humidityFactor), min(flatFactor, inlandFactor));
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
    std::vector<float> erosionNoise(numSamples);
    const BiomeNoiseGrids grids = {
        .temperature = temperatureNoise.data(),
        .humidity = humidityNoise.data(),
        .peak = peakNoise.data(),
        .inland = inlandNoise.data(),
        .erosion = erosionNoise.data(),
    };

    const vec2 texelCentersStartXZ = vec2(originBlocksXZ_WS) + texelSizeBlocks * 0.5f;
    fillGrids(grids, texelCentersStartXZ, numTexels, static_cast<float>(texelSizeBlocks));
    const auto oases = OasisShaping::makeContext(originBlocksXZ_WS, glm::ivec2(numTexels * texelSizeBlocks));

    for (uint32_t idx = 0; idx < numSamples; ++idx)
    {
        outBiomes[idx] = biomeFromNoise(noiseAt(grids, idx));
        const vec2 pos = texelCentersStartXZ + vec2(idx % numTexels.x, idx / numTexels.x) * static_cast<float>(texelSizeBlocks);
        if (OasisShaping::sample(pos, oases).vegetation) outBiomes[idx] = Biome::OASIS;
    }
}

} // namespace BiomeNoiseFields
