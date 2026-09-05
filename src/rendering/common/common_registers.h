// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

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
#define RT_REGISTER_LUT_SAMPLER 1 // linear clamp, for the sky atmosphere LUTs
#define RT_REGISTER_SKY_VIEW_SAMPLER 2 // linear, wrap in u; the sky-view LUT is periodic in azimuth
#define RT_REGISTER_BIOME_MAP_SAMPLER 3 // linear clamp, for the world-XZ biome color map

// =============================================
#define GBUFFER_REGISTER_SPACE 2
// =============================================

// u#
#define GBUFFER_REGISTER_GBUFFER_OUT 0

// =============================================
#define PT_REGISTER_SPACE 2
// =============================================

// b#
#define PT_REGISTER_PASS_CONSTANTS 0

// t#
#define PT_REGISTER_GBUFFER_IN 0
#define PT_REGISTER_PAIRING_TEXTURES_IN 1
#define PT_REGISTER_GBUFFER_PREV_IN 2
#define PT_REGISTER_RESERVOIRS_HISTORY_IN 3
#define PT_REGISTER_DUPLICATION_MAP_IN 4

// u#
#define PT_REGISTER_PATH_TRACING_RAW_BUFFER_OUT 0
#define PT_REGISTER_PT_DIFFUSE_ALBEDO_RAW_BUFFER_OUT 1
#define PT_REGISTER_RESERVOIRS_OUT 2
#define PT_REGISTER_RESERVOIRS_MERGED_OUT 3
#define PT_REGISTER_SHIFTED_OUT 4

// =============================================
#define RESTIR_REGISTER_SPACE 2
// =============================================

// t#
#define RESTIR_REGISTER_RESERVOIRS_MERGED_IN 0
#define RESTIR_REGISTER_SHIFTED_IN 1
#define RESTIR_REGISTER_PAIRING_TEXTURES_IN 2
#define RESTIR_REGISTER_DUPLICATION_MAP_IN 3

// u#
#define RESTIR_REGISTER_RESERVOIRS_HISTORY_OUT 0
#define RESTIR_REGISTER_PATH_TRACING_RAW_BUFFER_OUT 1
#define RESTIR_REGISTER_RESERVOIR_SEEDS_OUT 2

// =============================================
#define RESTIR_DUP_REGISTER_SPACE 2
// =============================================

// t#
#define RESTIR_DUP_REGISTER_RESERVOIR_SEEDS_IN 0

// u#
#define RESTIR_DUP_REGISTER_DUPLICATION_MAP_OUT 0

// =============================================
#define COLLECT_REGISTER_SPACE 2
// =============================================

// t#
#define COLLECT_REGISTER_PATH_TRACING_RAW_BUFFER_IN 0
#define COLLECT_REGISTER_PT_DIFFUSE_ALBEDO_RAW_BUFFER_IN 1

// =============================================
#define POSTPROCESS_REGISTER_SPACE 3
// =============================================

// s#
#define POSTPROCESS_REGISTER_TEX_SAMPLER 0

// =============================================
#define LIGHT_TREE_REGISTER_SPACE 4
// =============================================

// b#
#define LIGHT_TREE_REGISTER_CONSTANTS 0

// t# (consumed by path tracing raygen — light tree SRVs)
#define LIGHT_TREE_REGISTER_LIGHT_TREE_IN 0
#define LIGHT_TREE_REGISTER_LIGHT_TO_LEAF_IN 1

// u#
#define LIGHT_TREE_REGISTER_LIGHT_AUX_OUT 0
#define LIGHT_TREE_REGISTER_LIGHT_TO_LEAF_OUT 1
#define LIGHT_TREE_REGISTER_LIGHT_TREE_OUT 2
#define LIGHT_TREE_REGISTER_SCENE_BBOX_OUT 3 // RWByteAddressBuffer, 6 orderable-uint floats
#define LIGHT_TREE_REGISTER_MORTON_KEYS_OUT 4
#define LIGHT_TREE_REGISTER_MORTON_VALUES_OUT 5

// =============================================
#define WATER_DISPLACE_REGISTER_SPACE 5
// =============================================

// b#
#define WATER_DISPLACE_REGISTER_CONSTANTS 0

// u#
#define WATER_DISPLACE_REGISTER_VERTS_OUT 0

// =============================================
#define SKY_REGISTER_SPACE 6
// =============================================

// b#
#define SKY_REGISTER_CONSTANTS 0

// s#
#define SKY_REGISTER_LUT_SAMPLER 0

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
