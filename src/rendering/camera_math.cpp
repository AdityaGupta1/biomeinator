// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "camera_math.h"

#include <cmath>

namespace CameraMath
{

void normalize(SplitPosition& position)
{
    for (int component = 0; component < 3; ++component)
    {
        const int integerDelta = static_cast<int>(std::floor(position.fractional[component]));
        position.integer[component] += integerDelta;
        position.fractional[component] -= static_cast<float>(integerDelta);
    }
}

SplitPosition split(glm::vec3 position)
{
    SplitPosition result{ .fractional = position };
    normalize(result);
    return result;
}

void add(SplitPosition& position, glm::vec3 displacement)
{
    position.fractional += displacement;
    normalize(position);
}

glm::vec3 combine(const SplitPosition& position)
{
    return glm::vec3(position.integer) + position.fractional;
}

glm::vec3 relativeTo(const SplitPosition& position, glm::ivec3 integerOrigin)
{
    return glm::vec3(position.integer - integerOrigin) + position.fractional;
}

Basis makeBasis(float phi, float theta)
{
    const float cosPhi = std::cos(phi);
    const glm::vec3 forward =
        glm::normalize(glm::vec3(cosPhi * std::sin(theta), std::sin(phi), cosPhi * std::cos(theta)));
    const glm::vec3 worldUp(0.f, 1.f, 0.f);
    const glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));
    const glm::vec3 up = glm::normalize(glm::cross(right, forward));
    return { forward, right, up };
}

std::array<glm::vec3, 4> frustumSideNormals(const Basis& basis, float fovYRadians, float aspectRatio)
{
    const float tanHalfFovY = std::tan(fovYRadians * 0.5f);
    const float tanHalfFovX = tanHalfFovY * aspectRatio;
    return {
        glm::normalize(basis.right + basis.forward * tanHalfFovX),
        glm::normalize(-basis.right + basis.forward * tanHalfFovX),
        glm::normalize(basis.up + basis.forward * tanHalfFovY),
        glm::normalize(-basis.up + basis.forward * tanHalfFovY),
    };
}

} // namespace CameraMath
