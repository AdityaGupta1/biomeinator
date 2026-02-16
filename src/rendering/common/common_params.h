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

#include "common_enums.h"

#ifdef __cplusplus
#include <DirectXMath.h>

#define int3 DirectX::XMINT3

#define uint uint32_t
#define uint2 DirectX::XMUINT2

#define float2 DirectX::XMFLOAT2
#define float3 DirectX::XMFLOAT3

#define float4x4 DirectX::XMFLOAT4X4
#endif

struct HeapIndices
{
    struct
    {
        uint pathTracingTargetIdx;
        uint diffuseAlbedoTargetIdx;
        uint specularAlbedoTargetIdx;
        uint linearDepthTargetIdx;

        uint normalsAndRoughnessTargetIdx;
        uint motionTargetIdx;
        uint specularHitDistanceTargetIdx;
        uint debugTargetIdx;
    } uav;

    struct
    {
        uint pathTracingTargetIdx;
        uint diffuseAlbedoTargetIdx;
        uint specularAlbedoTargetIdx;
        uint linearDepthTargetIdx;

        uint normalsAndRoughnessTargetIdx;
        uint motionTargetIdx;
        uint specularHitDistanceTargetIdx;
        uint dlssOutputTargetIdx;

        uint debugTargetIdx;
        uint pad0;
        uint pad1;
        uint pad2;
    } srv;
};

struct ConstantParams
{
    uint rngSeed;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct CameraParams
{
    float4x4 worldToClipMat;
    float4x4 worldToPrevClipMat;

    float2 jitter;
    float2 prevJitter;

    float3 pos_WS;
    float nearPlane;

    float3 forward_WS;
    float farPlane;

    float3 right_WS;
    float tanHalfFovY;

    float3 up_WS;
    float prevTanHalfFovY;

    float3 prevPos_WS;
    uint pad0;

    float3 prevForward_WS;
    uint pad1;

    float3 prevRight_WS;
    uint pad2;

    float3 prevUp_WS;
    uint pad3;

    int3 globalInstanceOffset;
    uint pad4;

    int3 prevGlobalInstanceOffset;
    uint pad5;
};

struct SceneParams
{
    uint voxelMode;
    uint numAreaLights;
    uint pad0;
    uint pad1;
};

struct RenderParams
{
    uint frameNumber;
    uint accumulatedFrameNumber;
    uint maxPathDepth;
    uint samplingMode;

    uint tonemapping;
    uint preTonemappedColorSrvIdx;
    uint2 renderSize;

    uint pathSplitting;
    uint antialiasingMode;
    uint pad0;
    uint pad1;
};

struct DebugParams
{
    uint debugOutputSrvIdx;
    uint debugOutputNumChannels;
    float debugOutputScale;
    uint colorChunks;

    uint debugBool0;
    uint debugBool1;
    uint debugBool2;
    uint debugBool3;

    float debugFloat0;
    float debugFloat1;
    float debugFloat2;
    float debugFloat3;
};

#ifdef __cplusplus
#undef uint
#undef uint2

#undef float2
#undef float3

#undef float4x4
#endif

