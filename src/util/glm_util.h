// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "math.h"

#include <glm/glm.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/component_wise.hpp>

namespace glmUtil
{

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
