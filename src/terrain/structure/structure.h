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

#include "../block.h"

#include <glm/glm.hpp>
#include <vector>

enum StructureType
{
	OAK_TREE,
    SAGUARO_CACTUS,

	COUNT
};

struct Structure
{
    StructureType type;
    glm::ivec3 pos_WS;
    uint64_t seed;
};

inline constexpr uint32_t structureMaxChunkRadius = 1;

struct StructureBounds
{
    glm::ivec2 minDiffXZ;
    glm::ivec2 maxDiffXZ;

    StructureBounds() = default;
    StructureBounds(int diff);
};

struct StructureGen
{
    StructureType type;
    uint32_t gridCellSideLength;
    float minRadius;
};

namespace Structures
{

void init();

const StructureBounds& getStructureBounds(StructureType type);

} // namespace Structures
