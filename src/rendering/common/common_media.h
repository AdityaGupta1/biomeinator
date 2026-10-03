// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// Media that fill voxel cells. Each terrain face records the medium in front of it (the side its
// normal points to) and behind it; equal media mean the face is not a medium boundary.
#define MEDIUM_AIR 0
#define MEDIUM_WATER 1
#define MEDIUM_ICE 2
#define MEDIUM_GLASS 3
#define MEDIUM_COUNT 4
