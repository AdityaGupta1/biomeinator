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
#define COMMON_REGISTER_SPACE 0
// =============================================

// b#
#define COMMON_REGISTER_GLOBAL_PARAMS 0

// =============================================
#define RT_REGISTER_SPACE 1
// =============================================

// t#
#define RT_REGISTER_RAYTRACING_ACS 0
#define RT_REGISTER_VERTS 1
#define RT_REGISTER_IDXS 2
#define RT_REGISTER_INSTANCE_DATAS 3
#define RT_REGISTER_MATERIALS 4
#define RT_REGISTER_PER_TRI_DATAS 5
#define RT_REGISTER_AREA_LIGHTS 6
#define RT_REGISTER_AREA_LIGHT_SAMPLING_STRUCTURE 7

// s#
#define RT_REGISTER_TEX_SAMPLER 0

// =============================================
#define GBUFFER_REGISTER_SPACE 2
// =============================================

// u#
#define GBUFFER_REGISTER_GBUFFER_OUT 0
#define GBUFFER_REGISTER_RIS_SAMPLES_OUT 1

// =============================================
#define SPATIAL_REUSE_REGISTER_SPACE 2
// =============================================

// t#
#define SPATIAL_REUSE_REGISTER_RIS_SAMPLES_IN 0

// u#
#define SPATIAL_REUSE_REGISTER_RIS_SAMPLES_OUT 0

// =============================================
#define PT_REGISTER_SPACE 2
// =============================================

// t#
#define PT_REGISTER_GBUFFER_IN 0
#define PT_REGISTER_RIS_SAMPLES_IN 1

// u#
#define PT_REGISTER_PATH_TRACING_RAW_BUFFER_OUT 0

// =============================================
#define COLLECT_REGISTER_SPACE 2
// =============================================

// t#
#define COLLECT_REGISTER_PATH_TRACING_RAW_BUFFER_IN 0

// =============================================
#define POSTPROCESS_REGISTER_SPACE 3
// =============================================

// s#
#define POSTPROCESS_REGISTER_TEX_SAMPLER 0

// =============================================
// fake UAV slot for SER
// =============================================

#ifndef __cplusplus
#define NV_SHADER_EXTN_REGISTER_SPACE space1738
#define NV_SHADER_EXTN_SLOT u1738
#else
#define NV_SHADER_EXTN_REGISTER_SPACE 1738
#define NV_SHADER_EXTN_SLOT 1738
#endif

// =============================================
// helper macros
// =============================================

#ifndef __cplusplus
#define _REGISTER_IMPL(type, reg, spc) register(type##reg, space##spc)
#define REGISTER_U(reg, spc) _REGISTER_IMPL(u, reg, spc)
#define REGISTER_T(reg, spc) _REGISTER_IMPL(t, reg, spc)
#define REGISTER_B(reg, spc) _REGISTER_IMPL(b, reg, spc)
#define REGISTER_S(reg, spc) _REGISTER_IMPL(s, reg, spc)
#endif
