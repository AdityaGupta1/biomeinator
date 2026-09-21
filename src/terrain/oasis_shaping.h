// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <array>
#include <cstdint>

namespace OasisShaping
{
struct Pond
{
    glm::vec2 center;
    glm::vec2 direction;
    float radius;
    float aspect;
    uint32_t shapeSeed;
    std::array<glm::vec3, 3> basins; // local center and radius
    int level;
    bool active;
};
struct Context
{
    glm::ivec2 minCell;
    glm::ivec2 size;
    std::vector<Pond> ponds;
};
struct Sample
{
    float weight{ 0.f };
    float floorHeight{ 0.f };
    int waterLevel{ 0 };
    bool vegetation{ false };
    bool wet{ false };
};
void init(uint32_t seed);
Context makeContext(glm::ivec2 origin, glm::ivec2 extent);
Sample sample(glm::vec2 pos, const Context& context);
} // namespace OasisShaping
