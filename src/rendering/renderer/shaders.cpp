// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "shaders.h"

#include "debug.h"

#include <unordered_map>

#include "gpu_sort_downsweep_cs.fxh"
#include "gpu_sort_init_cs.fxh"
#include "gpu_sort_scan_cs.fxh"
#include "gpu_sort_upsweep_cs.fxh"
#include "light_tree/emitter_collect.cs.fxh"
#include "light_tree/light_buffer_clear.cs.fxh"
#include "light_tree/light_tree_bbox_reduce.cs.fxh"
#include "light_tree/light_tree_internal_levels.cs.fxh"
#include "light_tree/light_tree_leaf_populate.cs.fxh"
#include "light_tree/light_tree_morton_emit.cs.fxh"
#include "light_tree/light_tree_scene_bbox_reset.cs.fxh"
#include "nrc/nrc_query.rgs.fxh"
#include "nrc/nrc_resolve.cs.fxh"
#include "nrc/nrc_update.rgs.fxh"
#include "path_tracing/collect.cs.fxh"
#include "path_tracing/gbuffer.rgs.fxh"
#include "path_tracing/path_tracing.rgs.fxh"
#include "postprocess/debug_view.ps.fxh"
#include "postprocess/postprocess.ps.fxh"
#include "postprocess/postprocess.vs.fxh"
#include "tile_cache/rtsl_tile_cache_clear.cs.fxh"

#define REGISTER_SHADER(name) { #name, name##_shaderBytecode }

static const std::unordered_map<std::string_view, std::span<const unsigned char>> s_shaders = {
    REGISTER_SHADER(collect_cs),
    REGISTER_SHADER(debug_view_ps),
    REGISTER_SHADER(emitter_collect_cs),
    REGISTER_SHADER(gbuffer_rgs),
    REGISTER_SHADER(gpu_sort_downsweep_cs),
    REGISTER_SHADER(gpu_sort_init_cs),
    REGISTER_SHADER(gpu_sort_scan_cs),
    REGISTER_SHADER(gpu_sort_upsweep_cs),
    REGISTER_SHADER(light_buffer_clear_cs),
    REGISTER_SHADER(light_tree_bbox_reduce_cs),
    REGISTER_SHADER(light_tree_internal_levels_cs),
    REGISTER_SHADER(light_tree_leaf_populate_cs),
    REGISTER_SHADER(light_tree_morton_emit_cs),
    REGISTER_SHADER(light_tree_scene_bbox_reset_cs),
    REGISTER_SHADER(nrc_query_rgs),
    REGISTER_SHADER(nrc_resolve_cs),
    REGISTER_SHADER(nrc_update_rgs),
    REGISTER_SHADER(path_tracing_rgs),
    REGISTER_SHADER(postprocess_ps),
    REGISTER_SHADER(postprocess_vs),
    REGISTER_SHADER(rtsl_tile_cache_clear_cs),
};

std::span<const unsigned char> getShader(std::string_view name)
{
    const auto it = s_shaders.find(name);
    ASSERT(it != s_shaders.end(), "Shader not found");
    return it->second;
}
