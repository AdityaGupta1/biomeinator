// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include <array>

#include <glm/glm.hpp>

namespace CameraMath
{

struct SplitPosition
{
    glm::ivec3 integer{};
    glm::vec3 fractional{};
};

void normalize(SplitPosition& position);
SplitPosition split(glm::vec3 position);
void add(SplitPosition& position, glm::vec3 displacement);
glm::vec3 combine(const SplitPosition& position);
glm::vec3 relativeTo(const SplitPosition& position, glm::ivec3 integerOrigin);

struct Basis
{
    glm::vec3 forward;
    glm::vec3 right;
    glm::vec3 up;
};

Basis makeBasis(float phi, float theta);
std::array<glm::vec3, 4> frustumSideNormals(const Basis& basis, float fovYRadians, float aspectRatio);

} // namespace CameraMath
