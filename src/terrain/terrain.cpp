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

#include "terrain.h"

#include "block.h"
#include "terrain_materials.h"

namespace Terrain
{

static Scene* scene;

void init(Scene* scene)
{
    Terrain::scene = scene;

    TerrainMaterials::init(scene);

    Blocks::init();
}

static bool didAddChunk = false;
static std::unique_ptr<Chunk> chunk;

void update()
{
    if (!didAddChunk)
    {
        chunk = std::make_unique<Chunk>(glm::ivec2(0, 0));
        chunk->generateBlocks();
        chunk->createInstance(scene);

        didAddChunk = true;
    }

    // TODO
}

} // namespace Terrain
