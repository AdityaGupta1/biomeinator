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

#include "common_preamble.h"

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

// s#
#define RT_REGISTER_TEX_SAMPLER 0

// =============================================
#define GBUFFER_REGISTER_SPACE 2
// =============================================

// u#
#define GBUFFER_REGISTER_GBUFFER 0

// =============================================
#define PT_REGISTER_SPACE 2
// =============================================

// t#
#define PT_REGISTER_GBUFFER 0
#define PT_REGISTER_PER_TRI_DATAS 1
#define PT_REGISTER_AREA_LIGHTS 2
#define PT_REGISTER_AREA_LIGHT_SAMPLING_STRUCTURE 3

// =============================================
#define POSTPROCESS_REGISTER_SPACE 2
// =============================================

// s#
#define POSTPROCESS_REGISTER_TEX_SAMPLER 0

// =============================================
// helper macros
// =============================================

#if _hlsl
#define _REGISTER_IMPL(type, reg, spc) register(type##reg, space##spc)
#define REGISTER_U(reg, spc) _REGISTER_IMPL(u, reg, spc)
#define REGISTER_T(reg, spc) _REGISTER_IMPL(t, reg, spc)
#define REGISTER_B(reg, spc) _REGISTER_IMPL(b, reg, spc)
#define REGISTER_S(reg, spc) _REGISTER_IMPL(s, reg, spc)
#endif
