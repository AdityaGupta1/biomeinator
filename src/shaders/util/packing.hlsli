#pragma once

uint packFloat2toUint(float2 v)
{
    return (f32tof16(v.x) << 16) | (f32tof16(v.y));
}

float2 unpackUintToFloat2(uint v)
{
    return float2(f16tof32(v >> 16), f16tof32(v));
}
