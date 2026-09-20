// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "common_enums.h"

#ifdef __cplusplus
#include <DirectXMath.h>

#define int2 DirectX::XMINT2
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
        uint depthTargetIdx;

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
        uint depthTargetIdx;

        uint normalsAndRoughnessTargetIdx;
        uint motionTargetIdx;
        uint specularHitDistanceTargetIdx;
        uint dlssOutputTargetIdx;

        uint debugTargetIdx;
        uint transmittanceLutIdx;
        uint skyViewLutIdx;
        uint biomeMapIdx;

        uint cloudOccupancyIdx;
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
    uint cameraUnderwater;
    uint pad0;

    int3 voxelBoundsMin_WS;
    uint pad1;

    int3 voxelBoundsMax_WS;
    uint pad2;

    int2 biomeMapOriginBlocksXZ_WS;
    uint biomeMapTexelsPerSide;
    uint pad3;
};

struct CloudSettings
{
    float coverage;
    float extinction;
    float baseHeight;
    float thickness;

    float cellSize;
    float patternScale;
    float drawDistance;
    float shadowDistance;

    float ambient;
    float phaseG;
    float2 windDelta; // blocks moved since the previous frame, for motion vectors

    float multiScatterStrength;
    uint samples;
    uint seed;
    uint enableClouds;

    // Wind translation in blocks, split like the camera position so it stays exact as animTime grows
    int2 windOffsetInt;
    float2 windOffsetFrac;
};

// One frustum side plane through the camera; padded to the 16 bytes an array element takes in a
// constant buffer
struct WaveFadeFrustumNormal
{
    float3 normal_WS; // inward
    float pad0;
};

// Where water waves animate: the amplitude fades to rest height with distance from the camera
// and outside the padded view frustum; see waveFade in water_waves.hlsli
struct WaveFadeParams
{
    float3 cameraPos_WS; // absolute, not relative to globalInstanceOffset
    float fadeStart;

    float fadeEnd;
    float pad0;
    float pad1;
    float pad2;

    WaveFadeFrustumNormal frustumNormals_WS[4];
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

    uint doPathSplitting;
    uint antialiasingMode;
    uint refractionIndirectPassthrough;
    float mipBias;

    float animTime; // can be frozen by animTimePaused setting
    float waveTime; // animTime wrapped to WATER_WAVE_PERIOD_SECONDS, for the sine wave model
    float prevWaveTime;
    float fogSigmaS;

    float fogScaleHeight;
    float fogG;
    uint fogMarchSteps;
    float fogAmbientStrength;

    float skyStrength;
    uint pad0;
    uint pad1;
    uint pad2;

    CloudSettings cloudSettings;

    WaveFadeParams waveFade;
};

struct SharcParams
{
    uint enabled;
    uint capacity;
    uint downscale;
    uint frameIndex;

    float sceneScale;
    float roughnessMin;
    uint accumulationFrames;
    uint staleFrames;

    float3 cameraPosition;
    uint debugMode; // 0 beauty, 1 query hits, 2 bounce count, 3 hash grid, 4 cached radiance

    float3 cameraPositionPrev;
    uint padding1;

    int3 originDelta; // renderer origin minus stable cache origin
    uint padding;
};

struct RtslParams
{
    uint treeLeafBase;  // M - 1, or 0 if no light tree built / scene has no area lights
    uint treeLeafCount; // M (pow2-rounded numAreaLights), 0 = empty / disabled
    uint pad0;
    uint pad1;
};

struct DebugParams
{
    uint debugOutputSrvIdx;
    uint debugOutputNumChannels;
    float debugOutputScale;
    uint debugViewApplyTonemap;

    uint debugBool0;
    uint debugBool1;
    uint debugBool2;
    uint debugBool3;

    float debugFloat0;
    float debugFloat1;
    float debugFloat2;
    float debugFloat3;

    uint colorChunks;
    uint pad0;
    uint pad1;
    uint pad2;
};

#ifdef __cplusplus
#undef int2
#undef int3

#undef uint
#undef uint2

#undef float2
#undef float3

#undef float4x4
#endif
