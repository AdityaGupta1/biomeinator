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

namespace FN = FastNoise;

inline constexpr ClimateVector climateVecScales = {
    .temperature = 1.0f / 2.0f,
    .rainfall = 1.0f / 2.0f,
    .humidity = 1.0f / 2.0f,
    .altitude = 1.0f / chunkSizeY,
};

float ClimateVector::distance2(const ClimateVector& other) const
{
    return (temperature - other.temperature) * (temperature - other.temperature) * climateVecScales.temperature +
           (rainfall - other.rainfall) * (rainfall - other.rainfall) * climateVecScales.rainfall +
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
            .rainfall = 0.0f,
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
            .rainfall = -0.2f,
            .humidity = -1.0f,
            .altitude = 110.0f,
        };
        BIOME_DATA_BY_NAME(DESERT).heightFn = fnAdd;
        BIOME_DATA_BY_NAME(MOUNTAINS).topBlocks = {
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
        fnMul->SetRHS(12.0f);
        auto fnAdd = FN::New<FN::Add>();
        fnAdd->SetLHS(fnMul);
        fnAdd->SetRHS(125.0f);

        BIOME_DATA_BY_NAME(FOREST).climateVec = {
            .temperature = -0.1f,
            .rainfall = 0.2f,
            .humidity = 0.2f,
            .altitude = 125.0f,
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
        fnMul->SetRHS(30.0f);
        auto fnAdd = FN::New<FN::Add>();
        fnAdd->SetLHS(fnMul);
        fnAdd->SetRHS(180.0f);

        BIOME_DATA_BY_NAME(MOUNTAINS).climateVec = {
            .temperature = -0.4f,
            .rainfall = -0.1f,
            .humidity = -0.4f,
            .altitude = 180.0f,
        };
        BIOME_DATA_BY_NAME(MOUNTAINS).heightFn = fnAdd;
        BIOME_DATA_BY_NAME(MOUNTAINS).topBlocks = {
            .top = Block::STONE,
            .mid = Block::STONE,
        };
    }
}

const BiomeData& getBiomeData(Biome biome)
{
    return BIOME_DATA(biome);
}

} // namespace Biomes