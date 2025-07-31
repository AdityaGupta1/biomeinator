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

struct RandomSampler
{
    uint seed;

    [mutating]
    uint nextUint()
    {
        seed = hash(seed);
        return seed;
    }

    [mutating]
    float nextFloat()
    {
        return (nextUint() & 0x00FFFFFF) / 16777216.0;
    }

    [mutating]
    float2 nextFloat2()
    {
        return float2(nextFloat(), nextFloat());
    }

    [mutating]
    float3 nextFloat3()
    {
        return float3(nextFloat(), nextFloat(), nextFloat());
    }
};

RandomSampler initRandomSampler(uint seed)
{
    RandomSampler randomSampler;
    randomSampler.seed = hash(seed);
    return randomSampler;
}

RandomSampler initRandomSampler2(uint2 seed)
{
    return initRandomSampler(seed.x ^ hash(seed.y));
}

RandomSampler initRandomSampler3(uint3 seed)
{
    return initRandomSampler(seed.x ^ hash(seed.y ^ hash(seed.z)));
}

RandomSampler initRandomSampler4(uint4 seed)
{
    return initRandomSampler(seed.x ^ hash(seed.y ^ hash(seed.z ^ hash(seed.w))));
}
