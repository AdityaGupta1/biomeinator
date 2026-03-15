# Radiance Cache Implementation - Task Tracking

## Status Overview

| Step | Description | Status |
|------|-------------|--------|
| 1 | Add RC Constants, Params, and Settings | Done |
| 2 | Create GPU Buffers | Not Started |
| 3 | Eviction/Clear Compute Shader + Pipeline | Not Started |
| 4 | Resolve Compute Shader + Pipeline | Not Started |
| 5 | Radiance Cache Utility Header (HLSL) | Not Started |
| 6 | RC Update Pass (Cache Training) | Not Started |
| 7 | Debug Visualization | Not Started |
| 8 | Cache Read in Main Render Pass | Not Started |
| 9 | Tuning and Polish | Not Started |

## Step 1: Add RC Constants, Params, and Settings

- [x] Add constants to `common_settings.h`
- [x] Add `RadianceCacheParams` struct to `common_params.h`
- [x] Add `rcParams` to `global_params.hlsli` cbuffer
- [x] Add `rcParams` pointer to `param_block_manager.h/.cpp`
- [x] Add settings to `settings_manager.cpp`
- [x] Sync RC settings to `rcParams` in `renderer.cpp` `beginFrame()`
- [x] Add ImGui controls in `renderer.cpp`

## Findings

(Notes and discoveries made during implementation will be logged here.)
