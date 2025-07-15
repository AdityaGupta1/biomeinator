#pragma once

#include "common_preamble.h"

// u#
#define REGISTER_RENDER_TARGET 0

// t#
#define REGISTER_RAYTRACING_ACS 0
#define REGISTER_VERTS 1
#define REGISTER_IDXS 2
#define REGISTER_INSTANCE_DATAS 3
#define REGISTER_MATERIALS 4
#define REGISTER_TEXTURES 5

// b#
#define REGISTER_GLOBAL_PARAMS 0

// s#
#define REGISTER_TEX_SAMPLER 0

#if _hlsl
#define _REGISTER_IMPL(type, reg) register(type##reg)
#define REGISTER_U(reg) _REGISTER_IMPL(u, reg)
#define REGISTER_T(reg) _REGISTER_IMPL(t, reg)
#define REGISTER_B(reg) _REGISTER_IMPL(b, reg)
#define REGISTER_S(reg) _REGISTER_IMPL(s, reg)
#endif
