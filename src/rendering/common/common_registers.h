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

// =============================================
#define PT_REGISTER_SPACE 2
// =============================================

// t#
#define PT_REGISTER_GBUFFER_IN 0

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
// these macros are wacky but they make it much easier to define resources in shaders
#define _REGISTER_CONCAT(a, b) a##b
#define _REGISTER_EXPAND(x) x
#define _REGISTER_IMPL_STEP3(type, regVal, spcVal) register(_REGISTER_CONCAT(type, regVal), _REGISTER_CONCAT(space, spcVal))
#define _REGISTER_IMPL_STEP2(type, regName, spcName) _REGISTER_IMPL_STEP3(type, _REGISTER_EXPAND(regName), _REGISTER_EXPAND(spcName))
#define _REGISTER_IMPL_STEP1(type, prefix, param) _REGISTER_IMPL_STEP2(type, prefix##_REGISTER_##param, prefix##_REGISTER_SPACE)
#define REGISTER_U(prefix, param) _REGISTER_IMPL_STEP1(u, prefix, param)
#define REGISTER_T(prefix, param) _REGISTER_IMPL_STEP1(t, prefix, param)
#define REGISTER_B(prefix, param) _REGISTER_IMPL_STEP1(b, prefix, param)
#define REGISTER_S(prefix, param) _REGISTER_IMPL_STEP1(s, prefix, param)
#endif
