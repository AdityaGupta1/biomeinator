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

// =============================================
// RTSL SCREEN-SPACE TILE CACHE
// =============================================

// Screen tile edge in pixels. Tile index = pixel.xy / RTSL_TILE_PIXELS.
#define RTSL_TILE_PIXELS 8
// Log-depth sub-buckets per tile, so a near surface is not evicted by a far one.
#define RTSL_TILE_SUB_BUCKETS 4
// Buffer-sizing slot count per sub-bucket. The runtime active count
// (rtslCacheLightsPerCell) is clamped to this; K_MAX only sizes the allocation.
#define RTSL_LIGHT_CACHE_K_MAX 32

