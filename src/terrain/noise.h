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

#include <glm/glm.hpp>

// https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
inline glm::uint hash(glm::uint seed)
{
    glm::uint state = seed * 747796405u + 2891336453u;
    glm::uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

inline float rand1(glm::uint seed)
{
    return (hash(seed) & 0x00FFFFFF) / 16777216.f;
}

inline float rand1(glm::uvec2 seed)
{
    return rand1(seed.x ^ hash(seed.y));
}
