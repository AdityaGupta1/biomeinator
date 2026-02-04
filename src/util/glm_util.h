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
