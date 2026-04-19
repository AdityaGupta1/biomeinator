// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// =============================================
// COLLECT
// =============================================

#define COLLECT_WORKGROUP_SIZE_X 16
#define COLLECT_WORKGROUP_SIZE_Y 16

// =============================================
// RADIANCE CACHE
// =============================================

#define RC_TABLE_SIZE (1u << 22)
#define RC_WORKGROUP_SIZE 256
#define RC_UPDATE_SCALE 5
#define RC_TARGET_PIXEL_WIDTH 12
