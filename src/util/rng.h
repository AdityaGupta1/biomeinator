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

#pragma once

#include <glm/glm.hpp>

// https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
constexpr inline uint32_t hash(uint32_t seed)
{
    uint32_t state = seed * 747796405u + 2891336453u;
    uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

struct RandomNumberGenerator
{
    uint32_t seed;

    inline uint32_t nextUint()
    {
        seed = hash(seed);
        return seed;
    }

    inline float nextFloat()
    {
        return (nextUint() & 0x00FFFFFF) / 16777216.f;
    }

    inline float nextFloat(float max)
    {
        return nextFloat() * max;
    }

    inline float nextFloat(float min, float max)
    {
        return min + nextFloat(max - min);
    }

    inline float nextFloatAbs(float absMax)
    {
        return nextFloat(-absMax, absMax);
    }

    inline glm::vec2 nextFloat2()
    {
        return glm::vec2(nextFloat(), nextFloat());
    }

    inline glm::vec3 nextFloat3()
    {
        return glm::vec3(nextFloat(), nextFloat(), nextFloat());
    }

    inline bool chance(float p)
    {
        return nextFloat() < p;
    }

    inline int nextInt(int max)
    {
        return static_cast<int>(max * nextFloat());
    }

    inline int nextInt(int min, int max)
    {
        return static_cast<int>(min + (max - min) * nextFloat());
    }

    inline int nextIntAbs(int max)
    {
        return nextInt(-max, max + 1);
    }
};

inline RandomNumberGenerator initRng(uint32_t seed)
{
    RandomNumberGenerator rng;
    rng.seed = seed;
    return rng;
}

inline RandomNumberGenerator initRng(uint32_t seed1, uint32_t seed2)
{
    return initRng(seed1 ^ hash(seed2));
}

inline RandomNumberGenerator initRng(uint32_t seed1, uint32_t seed2, uint32_t seed3)
{
    return initRng(seed1 ^ hash(seed2 ^ hash(seed3)));
}

inline RandomNumberGenerator initRng(uint32_t seed1, uint32_t seed2, uint32_t seed3, uint32_t seed4)
{
    return initRng(seed1 ^ hash(seed2 ^ hash(seed3 ^ hash(seed4))));
}
