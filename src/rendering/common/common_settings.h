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
#define RC_STALE_FRAME_THRESHOLD 32
#define RC_RADIANCE_SCALE 1024.0
#define RC_SAMPLE_MULTIPLIER 1024
#define RC_DECAY 0.97
#define RC_UPDATE_SCALE 5
