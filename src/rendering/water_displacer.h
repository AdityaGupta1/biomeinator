// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "rendering/dxr_includes.h"

#include <vector>

#include <glm/glm.hpp>

// Compute pass that displaces WATER_TOP verts in place with the analytic wave function
// (see shaders/common/water_waves.hlsli).
namespace WaterDisplacer
{

struct DispatchInputs
{
    uint32_t vertsBufferOffset{ 0 }; // in verts
    uint32_t vertCount{ 0 };
    int32_t transformOffsetX{ 0 };
    int32_t transformOffsetZ{ 0 };
};

void init();

// The verts buffer must be in UNORDERED_ACCESS state; the caller owns the state
// transitions and the UAV barrier after the dispatches.
void dispatch(ID3D12GraphicsCommandList4* cmdList,
              D3D12_GPU_VIRTUAL_ADDRESS dev_vertsAddress,
              float time,
              const std::vector<DispatchInputs>& allInputs);

void destroy();

// Height of the displaced water top surface above its rest level at a point inside a block,
// sampled the way the mesh renders it: the wave function evaluated at the four block-corner
// verts and interpolated across the two top-face triangles (see the +y face in chunk.cpp's
// cubeFaceVertPositions), whose shared diagonal runs from local (0, 0) to (1, 1) in XZ.
float sampleMeshWaveOffsetY(glm::ivec2 blockXZ_WS, glm::vec2 blockFraction, float time);

} // namespace WaterDisplacer
