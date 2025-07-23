#pragma once

#include "common_preamble.h"

// =============================================
#define REGISTER_SPACE_BUFFERS 0
// =============================================

// t#
#define REGISTER_RAYTRACING_ACS 0
#define REGISTER_VERTS 1
#define REGISTER_IDXS 2
#define REGISTER_INSTANCE_DATAS 3
#define REGISTER_MATERIALS 4
#define REGISTER_AREA_LIGHTS 5
#define REGISTER_AREA_LIGHT_SAMPLING_STRUCTURE 6

// b#
#define REGISTER_GLOBAL_PARAMS 0

// =============================================
#define REGISTER_SPACE_TEXTURES 1
// =============================================

#define MAX_NUM_TEXTURES 8

// u#
#define REGISTER_RENDER_TARGET 0

// t#
#define REGISTER_TEXTURES 0

// s#
#define REGISTER_TEX_SAMPLER 0

#if _hlsl
#define _REGISTER_IMPL(type, reg, spc) register(type##reg, space##spc)
#define REGISTER_U(reg, spc) _REGISTER_IMPL(u, reg, spc)
#define REGISTER_T(reg, spc) _REGISTER_IMPL(t, reg, spc)
#define REGISTER_B(reg, spc) _REGISTER_IMPL(b, reg, spc)
#define REGISTER_S(reg, spc) _REGISTER_IMPL(s, reg, spc)
#endif
