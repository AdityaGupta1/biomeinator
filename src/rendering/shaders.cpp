// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "shaders.h"

#include "debug.h"

#include <unordered_map>

#include "collect.cs.fxh"
#include "debug_view.ps.fxh"
#include "gbuffer.rgs.fxh"
#include "path_tracing.rgs.fxh"
#include "postprocess.vs.fxh"
#include "postprocess.ps.fxh"
#include "rc_evict.cs.fxh"
#include "rc_resolve.cs.fxh"
#include "rc_update.rgs.fxh"

#define REGISTER_SHADER(name) { #name, name##_shaderBytecode }

static const std::unordered_map<std::string_view, std::span<const unsigned char>> s_shaders = {
    REGISTER_SHADER(collect_cs),
    REGISTER_SHADER(debug_view_ps),
    REGISTER_SHADER(gbuffer_rgs),
    REGISTER_SHADER(path_tracing_rgs),
    REGISTER_SHADER(postprocess_vs),
    REGISTER_SHADER(postprocess_ps),
    REGISTER_SHADER(rc_evict_cs),
    REGISTER_SHADER(rc_resolve_cs),
    REGISTER_SHADER(rc_update_rgs),
};

std::span<const unsigned char> getShader(std::string_view name)
{
    const auto it = s_shaders.find(name);
    ASSERT(it != s_shaders.end(), "Shader not found");
    return it->second;
}
