// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "debug.h"
#include "math.h"

#include <charconv>
#include <string_view>

#include <glm/glm.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/component_wise.hpp>

namespace glmUtil
{

// Decodes an "rrggbb" hex color (leading '#' optional, case-insensitive) into [0, 1] rgb
inline glm::vec3 colorFromHex(std::string_view hex)
{
    if (!hex.empty() && hex.front() == '#')
    {
        hex.remove_prefix(1);
    }
    ASSERT(hex.size() == 6);
    uint32_t packed = 0;
    [[maybe_unused]] const auto result = std::from_chars(hex.data(), hex.data() + hex.size(), packed, 16);
    ASSERT(result.ec == std::errc() && result.ptr == hex.data() + hex.size());
    return glm::vec3((packed >> 16) & 0xff, (packed >> 8) & 0xff, packed & 0xff) / 255.f;
}

inline int chebyshevDistance(glm::ivec2 a, glm::ivec2 b)
{
    return glm::compMax(glm::abs(a - b));
}

inline glm::ivec2 floorDiv(const glm::ivec2& a, const glm::ivec2& d)
{
    return {
        MathUtil::floorDiv(a.x, d.x),
        MathUtil::floorDiv(a.y, d.y),
    };
}

}
