// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "rendering/dxr_includes.h"

#include <vector>

#include "common/common_params.h"

#include <glm/glm.hpp>

// Compute pass that displaces WATER_TOP verts in place with the analytic wave function
// (see shaders/common/water_waves.hlsli).
class ToFreeList;

namespace WaterDisplacer
{

struct DispatchInputs
{
    uint32_t vertsBufferOffset{ 0 }; // in verts
    uint32_t vertCount{ 0 };
    glm::ivec3 transformOffset{ 0, 0, 0 };
    float waveScale{ 1.f }; // 0 flattens a chunk leaving the animated set
};

void init();

// The verts buffer must be in UNORDERED_ACCESS state; the caller owns the state
// transitions and the UAV barrier after the dispatches.
void dispatch(ID3D12GraphicsCommandList4* cmdList,
              ToFreeList& toFreeList,
              D3D12_GPU_VIRTUAL_ADDRESS dev_vertsAddress,
              float waveTime,
              const WaveFadeParams& waveFade,
              const std::vector<DispatchInputs>& allInputs);

void destroy();

// Height of the displaced water top surface above its rest level at a point inside a block,
// sampled to match the actual mesh geometry (by sampling corners and interpolating).
float sampleMeshWaveOffsetY(glm::ivec2 blockXZ_WS, glm::vec2 blockFraction, float waveTime);

} // namespace WaterDisplacer
