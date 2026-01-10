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

#include "block.h"
#include "scene/scene.h"

#include <array>
#include <glm/glm.hpp>

#define CHUNK_SIZE_X 16
#define CHUNK_SIZE_Y 16
#define CHUNK_SIZE_Z 256

class Chunk
{
private:
    const glm::ivec2 chunkPos{ 0, 0 };

    std::array<Block, CHUNK_SIZE_X * CHUNK_SIZE_Y * CHUNK_SIZE_Z> blocks{};

    Instance* instance;

public:
    Chunk(glm::ivec2 chunkPos);

    void generateBlocks();

    void createInstance(Scene* scene);

    static glm::uint blockPosToIdx(glm::uvec3 blockPos);
};
