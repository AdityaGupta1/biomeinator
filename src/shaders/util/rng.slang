#pragma once

uint hash(uint s)
{
    s ^= 2747636419u;
    s *= 2654435769u;
    s ^= s >> 16;
    s *= 2654435769u;
    s ^= s >> 16;
    s *= 2654435769u;
    return s;
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
