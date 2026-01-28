/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2026 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "biome.h"

#include "../chunk.h"

#include <array>
#include <limits>

#include <glm/glm.hpp>

namespace FN = FastNoise;

inline constexpr ClimateVector climateVecScales = {
    .temperature = 1.f / (2.f * 2.f),
    .precipitation = 1.f / (2.f * 2.f),
    .humidity = 1.0f / (2.f * 2.f),
    .altitude = 1.0f / (chunkSizeY * chunkSizeY),
};

float ClimateVector::distance2(const ClimateVector& other) const
{
    return (temperature - other.temperature) * (temperature - other.temperature) * climateVecScales.temperature +
           (precipitation - other.precipitation) * (precipitation - other.precipitation) * climateVecScales.precipitation +
           (humidity - other.humidity) * (humidity - other.humidity) * climateVecScales.humidity +
           (altitude - other.altitude) * (altitude - other.altitude) * climateVecScales.altitude;
}

namespace Biomes
{

static std::array<BiomeData, static_cast<size_t>(Biome::COUNT)> biomeDatas;

#define BIOME_DATA(biome) biomeDatas[static_cast<size_t>(biome)]
#define BIOME_DATA_BY_NAME(biomeName) biomeDatas[static_cast<size_t>(Biome::biomeName)]

void init()
{
    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(509324019);
        fnSimplex->SetScale(250.0f);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnSimplex);
        fnFractal->SetOctaveCount(4);
        auto fnMul = FN::New<FN::Multiply>();
        fnMul->SetLHS(fnFractal);
        fnMul->SetRHS(7.5f);
        auto fnAdd = FN::New<FN::Add>();
        fnAdd->SetLHS(fnMul);
        fnAdd->SetRHS(120.0f);

        BIOME_DATA_BY_NAME(PLAINS).climateVec = {
            .temperature = 0.0f,
            .precipitation = 0.0f,
            .humidity = 0.0f,
            .altitude = 120.0f,
        };
        BIOME_DATA_BY_NAME(PLAINS).heightFn = fnAdd;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(60924912);
        fnSimplex->SetScale(400.0f);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnSimplex);
        fnFractal->SetOctaveCount(2);
        auto fnMul = FN::New<FN::Multiply>();
        fnMul->SetLHS(fnFractal);
        fnMul->SetRHS(9.0f);
        auto fnAdd = FN::New<FN::Add>();
        fnAdd->SetLHS(fnMul);
        fnAdd->SetRHS(110.0f);

        BIOME_DATA_BY_NAME(DESERT).climateVec = {
            .temperature = 1.0f,
            .precipitation = -0.2f,
            .humidity = -1.0f,
            .altitude = 110.0f,
        };
        BIOME_DATA_BY_NAME(DESERT).heightFn = fnAdd;
        BIOME_DATA_BY_NAME(DESERT).topBlocks = {
            .top = Block::SAND,
            .mid = Block::SANDSTONE,
        };
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(210393129);
        fnSimplex->SetScale(200.0f);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnSimplex);
        fnFractal->SetOctaveCount(4);
        auto fnMul = FN::New<FN::Multiply>();
        fnMul->SetLHS(fnFractal);
        fnMul->SetRHS(15.0f);
        auto fnAdd = FN::New<FN::Add>();
        fnAdd->SetLHS(fnMul);
        fnAdd->SetRHS(130.0f);

        BIOME_DATA_BY_NAME(FOREST).climateVec = {
            .temperature = -0.1f,
            .precipitation = 0.2f,
            .humidity = 0.2f,
            .altitude = 130.0f,
        };
        BIOME_DATA_BY_NAME(FOREST).heightFn = fnAdd;
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(509324019);
        fnSimplex->SetScale(300.0f);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnSimplex);
        fnFractal->SetOctaveCount(4);
        auto fnMul = FN::New<FN::Multiply>();
        fnMul->SetLHS(fnFractal);
        fnMul->SetRHS(50.0f);
        auto fnAdd = FN::New<FN::Add>();
        fnAdd->SetLHS(fnMul);
        fnAdd->SetRHS(200.0f);

        BIOME_DATA_BY_NAME(MOUNTAINS).climateVec = {
            .temperature = -0.4f,
            .precipitation = -0.1f,
            .humidity = -0.4f,
            .altitude = 200.0f,
        };
        BIOME_DATA_BY_NAME(MOUNTAINS).heightFn = fnAdd;
        BIOME_DATA_BY_NAME(MOUNTAINS).topBlocks = {
            .top = Block::STONE,
            .mid = Block::STONE,
        };
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(509324019);
        fnSimplex->SetScale(200.0f);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnSimplex);
        fnFractal->SetOctaveCount(4);
        auto fnMul = FN::New<FN::Multiply>();
        fnMul->SetLHS(fnFractal);
        fnMul->SetRHS(10.0f);
        auto fnAdd = FN::New<FN::Add>();
        fnAdd->SetLHS(fnMul);
        fnAdd->SetRHS(120.0f);

        BIOME_DATA_BY_NAME(TUNDRA).climateVec = {
            .temperature = -0.7f,
            .precipitation = -0.3f,
            .humidity = -0.6f,
            .altitude = 120.0f,
        };
        BIOME_DATA_BY_NAME(TUNDRA).heightFn = fnAdd;
        BIOME_DATA_BY_NAME(TUNDRA).topBlocks = {
            .top = Block::SNOWY_GRASS,
            .mid = Block::DIRT,
        };
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(509324019);
        fnSimplex->SetScale(200.0f);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnSimplex);
        fnFractal->SetOctaveCount(4);
        auto fnMul = FN::New<FN::Multiply>();
        fnMul->SetLHS(fnFractal);
        fnMul->SetRHS(10.0f);
        auto fnAdd = FN::New<FN::Add>();
        fnAdd->SetLHS(fnMul);
        fnAdd->SetRHS(120.0f);

        BIOME_DATA_BY_NAME(TUNDRA).climateVec = {
            .temperature = -0.7f,
            .precipitation = -0.3f,
            .humidity = -0.6f,
            .altitude = 120.0f,
        };
        BIOME_DATA_BY_NAME(TUNDRA).heightFn = fnAdd;
        BIOME_DATA_BY_NAME(TUNDRA).topBlocks = {
            .top = Block::SNOWY_GRASS,
            .mid = Block::DIRT,
        };
    }

    {
        auto fnSimplex = FN::New<FN::Simplex>();
        fnSimplex->SetSeedOffset(509324019);
        fnSimplex->SetScale(500.0f);
        auto fnFractal = FN::New<FN::FractalFBm>();
        fnFractal->SetSource(fnSimplex);
        fnFractal->SetOctaveCount(4);
        auto fnMul = FN::New<FN::Multiply>();
        fnMul->SetLHS(fnFractal);
        fnMul->SetRHS(20.0f);
        auto fnAdd = FN::New<FN::Add>();
        fnAdd->SetLHS(fnMul);
        fnAdd->SetRHS(100.0f);

        BIOME_DATA_BY_NAME(ICE_FIELDS).climateVec = {
            .temperature = -0.85f,
            .precipitation = 0.1f,
            .humidity = -0.8f,
            .altitude = 100.0f,
        };
        BIOME_DATA_BY_NAME(ICE_FIELDS).heightFn = fnAdd;
        BIOME_DATA_BY_NAME(ICE_FIELDS).topBlocks = {
            .top = Block::SNOW,
            .mid = Block::ICE,
        };
    }
}

const BiomeData& getBiomeData(Biome biome)
{
    return BIOME_DATA(biome);
}

inline constexpr float farBiomeBlendWidth = 0.3f;

void getBiomeWeights(const ClimateVector& climateVec, BiomeWeight* biomeWeights)
{
    constexpr size_t paddedNumClosest = numClosestBiomes + 1;
    std::array<BiomeWeight, paddedNumClosest> closestBiomes;
    for (size_t i = 0; i < paddedNumClosest; ++i)
    {
        closestBiomes[i].biome = Biome::COUNT;
        closestBiomes[i].weight = std::numeric_limits<float>::max();
    }

    for (size_t biomeIdx = 0; biomeIdx < static_cast<size_t>(Biome::COUNT); ++biomeIdx)
    {
        const Biome biome = static_cast<Biome>(biomeIdx);
        const float dist2 = climateVec.distance2(BIOME_DATA(biome).climateVec);

        // insert if closer than the farthest biome so far
        if (dist2 < closestBiomes[paddedNumClosest - 1].weight)
        {
            closestBiomes[paddedNumClosest - 1].biome = biome;
            closestBiomes[paddedNumClosest - 1].weight = dist2;

            // maintain sorted order
            for (size_t i = paddedNumClosest - 1; i > 0 && closestBiomes[i].weight < closestBiomes[i - 1].weight; --i)
            {
                std::swap(closestBiomes[i], closestBiomes[i - 1]);
            }
        }
    }

    for (size_t i = 0; i < paddedNumClosest; ++i)
    {
        float newWeight = std::expf(-128.f * closestBiomes[i].weight);
        closestBiomes[i].weight = newWeight;
    }

    //const float farBiomeWeightDiff = closestBiomes[paddedNumClosest - 2].weight - closestBiomes[paddedNumClosest - 1].weight;
    //const float farBiomeWeightMultiplier = glm::smoothstep(0.f, farBiomeBlendWidth, farBiomeWeightDiff);
    //closestBiomes[paddedNumClosest - 2].weight *= farBiomeWeightMultiplier;

    float totalWeight = 0.f;
    for (size_t i = 0; i < numClosestBiomes; ++i)
    {
        totalWeight += closestBiomes[i].weight;
    }
    for (size_t i = 0; i < numClosestBiomes; ++i)
    {
        closestBiomes[i].weight /= totalWeight;
    }

    std::memcpy(biomeWeights, closestBiomes.data(), numClosestBiomes * sizeof(BiomeWeight));
}

} // namespace Biomes