/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2026 Aditya Gupta

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
