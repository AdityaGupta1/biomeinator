// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut vsMain(uint vertIdx : SV_VertexID)
{
    float2 pos2d = -1.f + 4.f * float2(vertIdx == 0, vertIdx == 2);

    VsOut vsOut;
    vsOut.pos = float4(pos2d, 0, 1);
    vsOut.uv = pos2d * float2(0.5f, -0.5f) + 0.5f;
    return vsOut;
}

