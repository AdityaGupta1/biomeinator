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
#include "nrc_resolve.cs.fxh"
#include "nrc_update.rgs.fxh"
#include "nrc_query.rgs.fxh"

#define REGISTER_SHADER(name) { #name, name##_shaderBytecode }

static const std::unordered_map<std::string_view, std::span<const unsigned char>> s_shaders = {
    REGISTER_SHADER(collect_cs),
    REGISTER_SHADER(debug_view_ps),
    REGISTER_SHADER(gbuffer_rgs),
    REGISTER_SHADER(path_tracing_rgs),
    REGISTER_SHADER(postprocess_vs),
    REGISTER_SHADER(postprocess_ps),
    REGISTER_SHADER(nrc_resolve_cs),
    REGISTER_SHADER(nrc_update_rgs),
    REGISTER_SHADER(nrc_query_rgs),
};

std::span<const unsigned char> getShader(std::string_view name)
{
    const auto it = s_shaders.find(name);
    ASSERT(it != s_shaders.end(), "Shader not found");
    return it->second;
}
