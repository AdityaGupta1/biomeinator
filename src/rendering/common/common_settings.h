// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// =============================================
// COLLECT
// =============================================

#define COLLECT_WORKGROUP_SIZE_X 16
#define COLLECT_WORKGROUP_SIZE_Y 16

// =============================================
// WATER DISPLACE
// =============================================

#define WATER_DISPLACE_WORKGROUP_SIZE 64

// Displacement wave parameters shared between water_waves.hlsli and the CPU mirror of
// waveHeight() in water_displacer.cpp. See water_waves.hlsli for the wave model description
// and the meaning of each strength/freq/speed triple; the shading-normal noise parameters
// live only there since the CPU never evaluates them.

#define WATER_SWELL_WAVE_COUNT 2
#define WATER_SWELL_STRENGTHS { 0.03f, 0.025f }
#define WATER_SWELL_FREQS { { 0.08f, 0.06f }, { -0.05f, 0.11f } }
#define WATER_SWELL_SPEEDS { 0.4f, 0.5f }

#define WATER_CHOP_WAVE_COUNT 3
#define WATER_CHOP_STRENGTHS { 0.02f, 0.015f, 0.01f }
#define WATER_CHOP_FREQS { { 0.8f, 0.6f }, { -0.5f, 1.3f }, { 1.2f, -0.4f } }
#define WATER_CHOP_SPEEDS { 0.55f, 0.85f, 0.7f }

#define WATER_SINE_CHOP_FREQS { { 0.0167f, 0.01f }, { -0.0067f, 0.02f } }
#define WATER_SINE_CHOP_SPEEDS { 0.1f, 0.13f }

// =============================================
// TERRAIN
// =============================================

#define SEA_LEVEL 125

// World-XZ biome color map: one texel covers this many blocks per axis
#define BIOME_MAP_BLOCKS_PER_TEXEL 8

// =============================================
// SKY ATMOSPHERE
// =============================================

#define SUN_PERIOD_SECONDS 1200.f

#define SKY_TRANSMITTANCE_LUT_WIDTH 256
#define SKY_TRANSMITTANCE_LUT_HEIGHT 64
#define SKY_TRANSMITTANCE_LUT_NUM_STEPS 40

#define SKY_MULTI_SCATTERING_LUT_SIZE 32
#define SKY_MULTI_SCATTERING_NUM_DIRS 64
#define SKY_MULTI_SCATTERING_NUM_STEPS 20

#define SKY_VIEW_LUT_WIDTH 200
#define SKY_VIEW_LUT_HEIGHT 100
#define SKY_VIEW_LUT_NUM_STEPS 30

#define SKY_WORKGROUP_SIZE_X 8
#define SKY_WORKGROUP_SIZE_Y 8

// =============================================
// LIGHT TREE
// =============================================

#define EMITTER_COLLECT_WORKGROUP_SIZE 64
#define LIGHT_BUFFER_CLEAR_WORKGROUP_SIZE 64

#define LIGHT_TREE_SCENE_BBOX_RESET_WORKGROUP_SIZE 64
#define LIGHT_TREE_BBOX_REDUCE_WORKGROUP_SIZE 256
#define LIGHT_TREE_MORTON_EMIT_WORKGROUP_SIZE 64
#define LIGHT_TREE_LEAF_POPULATE_WORKGROUP_SIZE 64
#define LIGHT_TREE_INTERNAL_LEVELS_WORKGROUP_SIZE 64

// Smallest leaf count for the perfect-binary light tree. Matches the Stage 1
// LIGHT_AUX_CAPACITY_FLOOR (256) so M = nextPow2(numAreaLights) is always >= 256
// and log2(M) >= 8.
#define LIGHT_TREE_LEAF_FLOOR 256

