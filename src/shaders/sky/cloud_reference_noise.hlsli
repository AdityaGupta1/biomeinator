// SPDX-License-Identifier: Apache-2.0
// Copyright 2011-2022 Blender Foundation
// Copyright 2026 Aditya Gupta
// HLSL adaptation of the required Blender 4.4.1 Cycles noise/hash/Voronoi
// routines. See external/_licenses/LICENSE_cycles.txt. Changes: reduced to
// the reference's 2D/3D normalized fBM and 2D Smooth F1; periodic XY lattice.
#pragma once

uint cloudRotate(uint v, uint n) { return (v << n) | (v >> (32u-n)); }
uint cloudHashFinal(uint a, uint b, uint c)
{
    c = (c ^ b) - cloudRotate(b,14); a = (a ^ c) - cloudRotate(c,11);
    b = (b ^ a) - cloudRotate(a,25); c = (c ^ b) - cloudRotate(b,16);
    a = (a ^ c) - cloudRotate(c,4); b = (b ^ a) - cloudRotate(a,14);
    return (c ^ b) - cloudRotate(b,24);
}
uint cloudHash2(uint2 v)
{
    const uint s = 0xdeadbeefu + 8u + 13u;
    return cloudHashFinal(s+v.x,s+v.y,s);
}
uint cloudHash3(uint3 v)
{
    const uint s = 0xdeadbeefu + 12u + 13u;
    return cloudHashFinal(s+v.x,s+v.y,s+v.z);
}
float cloudFloatHash(float2 p) { return float(cloudHash2(asuint(p))) / 4294967295.f; }

// Keep the central reference region's seeds intact; only the distant tile
// boundary wraps. Per-octave periods preserve the fBM and color offsets.
int2 cloudWrapCell(int2 p, int period, int2 center)
{
    int2 q = p - center + period/2;
    return (q % period + period) % period - period/2 + center;
}
float cloudGradient2(uint h, float2 p)
{
    h &= 7u;
    float u = h < 4u ? p.x : p.y;
    float v = 2.f * (h < 4u ? p.y : p.x);
    return ((h & 1u) ? -u : u) + ((h & 2u) ? -v : v);
}
float cloudGradient3(uint h, float3 p)
{
    h &= 15u;
    float u = h < 8u ? p.x : p.y;
    float v = h < 4u ? p.y : ((h == 12u || h == 14u) ? p.x : p.z);
    return ((h & 1u) ? -u : u) + ((h & 2u) ? -v : v);
}
// Evolve lattice gradients instead of translating the coordinates. Each site
// rotates at its own rate, so the field changes shape while wind remains a
// separate world-space translation. time=0 preserves the Blender reference.
float2 cloudEvolve2(float2 delta, uint seed, float time)
{
    if (time == 0.f) return delta;
    float s, c;
    sincos(time*(0.5f+float(seed & 1023u)/1024.f),s,c);
    return float2(c*delta.x-s*delta.y,s*delta.x+c*delta.y);
}
float3 cloudEvolve3(float3 delta, uint seed, float time)
{
    delta.xy = cloudEvolve2(delta.xy,seed,time);
    delta.yz = cloudEvolve2(delta.yz,seed >> 10u,time);
    return delta;
}
float cloudPerlin2(float2 p, int period, float2 center, float time)
{
    int2 cell = int2(floor(p));
    float2 f = frac(p), w = f*f*f*(f*(f*6.f-15.f)+10.f);
    float result = 0.f;
    [unroll] for (int y=0; y<2; ++y)
    [unroll] for (int x=0; x<2; ++x)
    {
        int2 o = int2(x,y);
        uint h = cloudHash2(uint2(cloudWrapCell(cell+o,period,int2(floor(center)))));
        float2 weight = lerp(1.f-w,w,float2(o));
        result += cloudGradient2(h,cloudEvolve2(f-float2(o),h,time))*weight.x*weight.y;
    }
    return result*0.6616f;
}
float cloudPerlin3(float3 p, int period, float time)
{
    int3 cell = int3(floor(p));
    float3 f = frac(p), w = f*f*f*(f*(f*6.f-15.f)+10.f);
    float result = 0.f;
    [unroll] for (int z=0; z<2; ++z)
    [unroll] for (int y=0; y<2; ++y)
    [unroll] for (int x=0; x<2; ++x)
    {
        int3 o = int3(x,y,z), c = cell+o;
        c.xy = cloudWrapCell(c.xy,period,int2(0,0));
        float3 weight = lerp(1.f-w,w,float3(o));
        uint h = cloudHash3(uint3(c));
        result += cloudGradient3(h,cloudEvolve3(f-float3(o),h,time))*weight.x*weight.y*weight.z;
    }
    return result*0.982f;
}
float cloudFbm2(float2 p, float2 offset, float frequency, float detail, float roughness, float time)
{
    float sum = 0.f, amplitude = 1.f, total = 0.f;
    int octave = 0;
    [loop] for (; octave<=int(detail); ++octave)
    {
        const float scale = float(1u << octave);
        sum += amplitude*cloudPerlin2((p+offset)*scale,int(16.f*frequency) << octave,offset*scale,time);
        total += amplitude;
        amplitude *= roughness;
    }
    float value = sum/total;
    if (frac(detail) > 0.f)
    {
        const float scale = float(1u << octave);
        float next = cloudPerlin2((p+offset)*scale,int(16.f*frequency) << octave,offset*scale,time);
        value = lerp(value,(sum+amplitude*next)/(total+amplitude),frac(detail));
    }
    return 0.5f + 0.5f*value;
}
float2 cloudReferenceWarp(float2 p, float frequency=2.5f, float detail=2.f, float roughness=0.5f, float time=0.f)
{
    // Noise Color R is Fac; G is seeded with offset 2 (not offset 1).
    float2 offset = 100.f + 100.f*float2(cloudFloatHash(float2(2,0)),cloudFloatHash(float2(2,1)));
    return float2(cloudFbm2(p*frequency,0.f,frequency,detail,roughness,time),
                  cloudFbm2(p*frequency,offset,frequency,detail,roughness,time));
}
float cloudReferenceFine(float3 p, float frequency=30.f, float detail=2.f, float roughness=0.5f, float time=0.f)
{
    float sum = 0.f, amplitude = 1.f, total = 0.f;
    int octave = 0;
    [loop] for (; octave<=int(detail); ++octave)
    {
        sum += amplitude*cloudPerlin3(p*(frequency*float(1u << octave)),int(16.f*frequency) << octave,time);
        total += amplitude;
        amplitude *= roughness;
    }
    float value = sum/total;
    if (frac(detail) > 0.f)
    {
        float next = cloudPerlin3(p*(frequency*float(1u << octave)),int(16.f*frequency) << octave,time);
        value = lerp(value,(sum+amplitude*next)/(total+amplitude),frac(detail));
    }
    return 0.5f + 0.5f*value;
}
float cloudSmoothF1(float2 p, float smoothness=1.f, float randomness=2.f/3.f, float time=0.f)
{
    int2 cell = int2(floor(p));
    float2 f = frac(p);
    float distance = 0.f;
    const float blendWidth = smoothness*0.5f;
    [loop] for (int y=-2; y<=2; ++y)
    [loop] for (int x=-2; x<=2; ++x)
    {
        int2 o = int2(x,y);
        float2 c = float2(cloudWrapCell(cell+o,16,int2(0,0)));
        float2 random = float2(cloudFloatHash(c),float(cloudHash3(asuint(float3(c,1.f))))/4294967295.f);
        if (time != 0.f)
        {
            // Continuous bounded trajectories; no reseeding or jumps at whole
            // times, and every point stays inside its original cell.
            float2 rate = 0.5f+float2(cloudFloatHash(c+17.3f),cloudFloatHash(c+91.7f));
            random = 0.5f+0.5f*sin(asin(clamp(2.f*random-1.f,-1.f,1.f))+time*rate);
        }
        float d = length(float2(o)+random*randomness-f);
        // Node Smoothness=1 -> internal 0.5, with Blender's cubic blend.
        float h = (x == -2 && y == -2) ? 1.f : (blendWidth > 0.f ?
            smoothstep(0.f,1.f,0.5f+0.5f*(distance-d)/blendWidth) : float(d < distance));
        distance = lerp(distance,d,h)-blendWidth*h*(1.f-h);
    }
    return distance;
}

// Cycles evaluates Color Ramps through a 256-interval table (257 entries). Interpolating the
// two enclosing entries matters for this steep height ramp near the base.
float cloudReferenceHeight(float mappedZ, float rampEnd=0.17727293f, float gain=6.1f)
{
    float f = saturate(mappedZ/10.f)*256.f;
    float a = floor(f)/256.f, b = min(floor(f)+1.f,256.f)/256.f;
    return gain*lerp(smoothstep(0.f,rampEnd,a),smoothstep(0.f,rampEnd,b),frac(f));
}
float cloudReferenceRamp(float value, float white=0.145454556f, float black=0.363636315f)
{
    float f = saturate(value)*256.f;
    float a = floor(f)/256.f, b = min(floor(f)+1.f,256.f)/256.f;
    return lerp(saturate((black-a)/(black-white)),
                saturate((black-b)/(black-white)),frac(f));
}


