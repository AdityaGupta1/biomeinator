// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

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

// dxc complains when calling nextFloat() directly on payload rng with access qualifiers, so use this function instead
float nextFloat(inout RandomNumberGenerator rng)
{
    return rng.nextFloat();
}

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
