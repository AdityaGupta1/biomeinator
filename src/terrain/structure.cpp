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

#include "structure.h"

#include "block.h"
#include "util/rng.h"

#include <array>

static void fillStructureBlocks_OAK_TREE(const Structure& structure, glm::ivec2 chunkPosBlocksXZ_WS, std::vector<Block>& blocks)
{

}

namespace Structures
{

using FillStructureFunc = void (*)(const Structure& structure, glm::ivec2 chunkPosBlocksXZ_WS, std::vector<Block>& blocks);
static std::array<FillStructureFunc, static_cast<size_t>(StructureType::COUNT)> fillStructureFuncs;

#define GET_FILL_STRUCTURE_FUNC_BY_NAME(structureName) fillStructureFuncs[static_cast<size_t>(StructureType::structureName)]
#define SET_FILL_STRUCTURE_FUNC(structureName) GET_FILL_STRUCTURE_FUNC_BY_NAME(structureName) = fillStructureBlocks_##structureName;

void init()
{
    SET_FILL_STRUCTURE_FUNC(OAK_TREE);
}

void fillStructureBlocks(const Structure* structures, uint32_t numStructures, glm::ivec2 chunkPosBlocksXZ_WS, std::vector<Block>& blocks)
{
    for (uint32_t i = 0; i < numStructures; i++)
    {
        const Structure& structure = structures[i];
        const FillStructureFunc fillStructureFunc = fillStructureFuncs[static_cast<size_t>(structure.type)];
        fillStructureFunc(structure, chunkPosBlocksXZ_WS, blocks);
    }
}

} // namespace Structures
