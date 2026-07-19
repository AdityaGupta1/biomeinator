// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// =============================================
// COLLECT
// =============================================

#define COLLECT_WORKGROUP_SIZE_X 16
#define COLLECT_WORKGROUP_SIZE_Y 16

// =============================================
// NRC RESOLVE
// =============================================

#define NRC_RESOLVE_WORKGROUP_SIZE_X 16
#define NRC_RESOLVE_WORKGROUP_SIZE_Y 16

// =============================================
// WATER DISPLACE
// =============================================

#define WATER_DISPLACE_WORKGROUP_SIZE 64

// =============================================
// SKY ATMOSPHERE
// =============================================

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

