// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "rendering/dxr_includes.h"
#include "rendering/common/common_cloud_settings.h"

#include <cstdint>

// Compute passes that generate the physically based sky LUTs (see shaders/sky/atmosphere.hlsli):
// a one-time transmittance LUT and a per-frame sky-view LUT, both consumed by
// shaders/light/dome_light.hlsli.
namespace SkyAtmosphere
{

void init();

// Records the one-time transmittance LUT generation (first call only) and the per-frame sky-view
// LUT update. Must run before the path trace pass each frame; leaves both LUTs in
// NON_PIXEL_SHADER_RESOURCE state. The descriptor heap must already be bound.
void dispatch(ID3D12GraphicsCommandList4* cmdList, float animTime, float cameraY,
              bool clouds, float coverage, float density, const CloudSettings& settings,
              D3D12_GPU_VIRTUAL_ADDRESS globalParams);
void resizeCloudView(uint32_t width, uint32_t height, uint32_t divisor);

uint32_t getTransmittanceLutSrvIdx();
uint32_t getSkyViewLutSrvIdx();
uint32_t getCloudNoiseSrvIdx();
uint32_t getCloudShapeSrvIdx();
uint32_t getCloudLightSrvIdx();
uint32_t getCloudViewSrvIdx();

void destroy();

} // namespace SkyAtmosphere
