/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

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

// https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
uint hash(uint seed)
{
    uint state = seed * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

struct RandomNumberGenerator
{
    uint seed;

    uint nextUint()
    {
        seed = hash(seed);
        return seed;
    }

    float nextFloat()
    {
        return (nextUint() & 0x00FFFFFF) / 16777216.f;
    }

    float2 nextFloat2()
    {
        return float2(nextFloat(), nextFloat());
    }

    float3 nextFloat3()
    {
        return float3(nextFloat(), nextFloat(), nextFloat());
    }
};

RandomNumberGenerator initRng(uint seed)
{
    RandomNumberGenerator rng;
    rng.seed = seed;
    return rng;
}

RandomNumberGenerator initRng(uint seed1, uint seed2)
{
    return initRng(seed1 ^ hash(seed2));
}

RandomNumberGenerator initRng(uint seed1, uint seed2, uint seed3)
{
    return initRng(seed1 ^ hash(seed2 ^ hash(seed3)));
}

RandomNumberGenerator initRng(uint seed1, uint seed2, uint seed3, uint seed4)
{
    return initRng(seed1 ^ hash(seed2 ^ hash(seed3 ^ hash(seed4))));
}
