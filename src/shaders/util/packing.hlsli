#pragma once

uint packFloat2toUint(float2 v)
{
    return f32tof16(v.x) | (f32tof16(v.y) << 16);
}

float2 unpackUintToFloat2(uint v)
{
    return float2(f16tof32(v), f16tof32(v >> 16));
}

uint packSnorm2ToUint(float2 v)
{
    const int2 i = int2(round(clamp(v, -1.f, 1.f) * 32767.f));
    return (uint(i.x) & 0xFFFF) | (uint(i.y) << 16);
}

float2 unpackUintToSnorm2(uint u)
{
    const int2 i = int2(u << 16, u) >> 16;
    return clamp(i / 32767.f, -1.f, 1.f);
}

// octahedron normal encoding code from https://knarkowicz.wordpress.com/2014/04/16/octahedron-normal-vector-encoding/
float2 octWrap(float2 v)
{
    return (1.f - abs(v.yx)) * select(v.xy >= 0.f, 1.f, -1.f);
}

uint octEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.f ? n.xy : octWrap(n.xy);
    n.xy = n.xy * 0.5f + 0.5f;
    return packSnorm2ToUint(n.xy);
}

float3 octDecode(uint u)
{
    const float2 f = unpackUintToSnorm2(u) * 2.f - 1.f;
    float3 n = float3(f.x, f.y, 1.f - abs(f.x) - abs(f.y));
    const float t = saturate(-n.z);
    n.xy += select(n.xy >= 0.f, -t, t);
    return normalize(n);
}
