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

#include "chunk.h"

#include "block.h"
#include "noise.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/common/common_structs.h"
#include "terrain_materials.h"

#include <DirectXMath.h>
#include <vector>

using namespace glm;
using namespace DirectX;

#define DEFAULT_TEX_NUM_BLOCKS_X 32
#define DEFAULT_TEX_NUM_BLOCKS_Y 32

Chunk::Chunk(ivec2 chunkPos)
	: chunkPos(chunkPos)
{}

void Chunk::generateBlocks()
{
    const ivec2 chunkBlockPos_WS = chunkPos * 16;

    for (uint z = 0; z < CHUNK_SIZE_XZ; ++z)
    {
        for (uint x = 0; x < CHUNK_SIZE_XZ; ++x)
        {
            const ivec2 blockPosXZ_WS = chunkBlockPos_WS + ivec2(x, z);
            const uint height = uint(64.f + 10.f * (sinf(blockPosXZ_WS.x * 0.1f) * cosf(blockPosXZ_WS.y * 0.1f)));

            for (uint y = 0; y < height; ++y)
            {
                const ivec3 blockPos_CS = ivec3(x, y, z);
                const ivec3 blockPos_WS = ivec3(blockPosXZ_WS.x, y, blockPosXZ_WS.y);
                this->blocks[blockPosToIdx(blockPos_CS)] = rand1(uvec3(blockPos_WS)) < 0.04f ? Block::LAMP : Block::STONE;
            }

            if (rand1(uvec2(blockPosXZ_WS)) < 0.02f && height < CHUNK_SIZE_Y)
            {
                this->blocks[blockPosToIdx(ivec3(x, height, z))] = Block::LAMP;
            }
        }
    }
}

static inline DirectX::XMFLOAT2 vec2ToDirectX(const glm::vec2& v)
{
    return { v.x, v.y };
}

static inline DirectX::XMFLOAT3 vec3ToDirectX(const glm::vec3& v)
{
    return { v.x, v.y, v.z };
}

void Chunk::createInstance(Scene* scene)
{
    ToFreeList toFreeList{};

    // TODO: will have to revisit this when implementing multithreading
    // could potentially be a case where the instances array/map/whatever is resized while some instances are still being worked on, so their data would be lost
    this->instance = scene->requestNewInstance(toFreeList);

    const ivec2 chunkBlockPos_WS = chunkPos * 16;
    const XMMATRIX transform = XMMatrixTranslation(
        static_cast<float>(chunkBlockPos_WS.x),
        0.f,
        static_cast<float>(chunkBlockPos_WS.y)
    );
    XMFLOAT3X4 instanceTransform;
    XMStoreFloat3x4(&instanceTransform, transform);

    std::vector<Vertex> verts;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> emissiveTriangleIndices;

    static constexpr ivec3 faceOffsets[6] = {
        ivec3(1, 0, 0),  // +x
        ivec3(-1, 0, 0), // -x
        ivec3(0, 1, 0),  // +y
        ivec3(0, -1, 0), // -y
        ivec3(0, 0, 1),  // +z
        ivec3(0, 0, -1), // -z
    };

    // TODO: extract this to a helper function
    auto isBlockAir = [&](ivec3 pos_CS) -> bool {
        if (pos_CS.x < 0 || pos_CS.x >= static_cast<int>(CHUNK_SIZE_XZ) ||
            pos_CS.y < 0 || pos_CS.y >= static_cast<int>(CHUNK_SIZE_Y) ||
            pos_CS.z < 0 || pos_CS.z >= static_cast<int>(CHUNK_SIZE_XZ))
        {
            // TODO: account for blocks in neighboring chunks
            return true;
        }
        return blocks[blockPosToIdx(uvec3(pos_CS))] == Block::AIR;
    };

    for (uint z = 0; z < CHUNK_SIZE_XZ; ++z)
    {
        for (uint x = 0; x < CHUNK_SIZE_XZ; ++x)
        {
            for (uint y = 0; y < CHUNK_SIZE_Y; ++y)
            {
                const uvec3 blockPos_CS(x, y, z);
                const Block block = blocks[blockPosToIdx(blockPos_CS)];
                if (block == Block::AIR)
                {
                    continue;
                }

                const BlockData& blockData = Blocks::getBlockData(block);

                for (uint faceIdx = 0; faceIdx < 6; ++faceIdx)
                {
                    const ivec3 neighborOffset = faceOffsets[faceIdx];
                    const ivec3 neighborPos_CS = ivec3(blockPos_CS) + neighborOffset;

                    if (!isBlockAir(neighborPos_CS))
                    {
                        continue;
                    }

                    const vec3 normal = vec3(neighborOffset);

                    static constexpr ivec3 allFaceVerts[24] = {
                        ivec3(1, 1, 0), ivec3(1, 1, 1), ivec3(1, 0, 1), ivec3(1, 0, 0), // +x
                        ivec3(0, 1, 1), ivec3(0, 1, 0), ivec3(0, 0, 0), ivec3(0, 0, 1), // -x
                        ivec3(1, 1, 1), ivec3(1, 1, 0), ivec3(0, 1, 0), ivec3(0, 1, 1), // +y
                        ivec3(0, 0, 1), ivec3(0, 0, 0), ivec3(1, 0, 0), ivec3(1, 0, 1), // -y
                        ivec3(1, 1, 1), ivec3(0, 1, 1), ivec3(0, 0, 1), ivec3(1, 0, 1), // +z
                        ivec3(0, 1, 0), ivec3(1, 1, 0), ivec3(1, 0, 0), ivec3(0, 0, 0)  // -z
                    };
                    const ivec3* faceVerts = allFaceVerts + (faceIdx * 4);

                    static constexpr uvec2 uvOffsets[4] = {
                        uvec2(1, 0),
                        uvec2(0, 0),
                        uvec2(0, 1),
                        uvec2(1, 1),
                    };

                    const uint32_t baseVertIdx = static_cast<uint32_t>(verts.size());
                    for (uint i = 0; i < 4; ++i)
                    {
                        const vec2 uv = (vec2(blockData.texCoords + uvOffsets[i])) /
                                        vec2(DEFAULT_TEX_NUM_BLOCKS_X, DEFAULT_TEX_NUM_BLOCKS_Y);
                        const vec3 vertPos_CS = vec3(ivec3(blockPos_CS) + faceVerts[i]);
                        verts.push_back({
                            vec3ToDirectX(vertPos_CS),
                            vec3ToDirectX(normal),
                            vec2ToDirectX(uv),
                        });
                    }

                    const uint32_t triangleIdx = static_cast<uint32_t>(indices.size() / 3);

                    indices.push_back(baseVertIdx + 0);
                    indices.push_back(baseVertIdx + 1);
                    indices.push_back(baseVertIdx + 2);
                    indices.push_back(baseVertIdx + 0);
                    indices.push_back(baseVertIdx + 2);
                    indices.push_back(baseVertIdx + 3);

                    if (blockData.emitsLight)
                    {
                        emissiveTriangleIndices.push_back(triangleIdx);
                        emissiveTriangleIndices.push_back(triangleIdx + 1);
                    }
                }
            }
        }
    }

    instance->setGeometry(instanceTransform, std::move(verts), std::move(indices));
    instance->setMaterialIdx(TerrainMaterials::getDefaultMaterialIdx());

    for (uint32_t triangleIdx : emissiveTriangleIndices)
    {
        instance->addAreaLight(triangleIdx);
    }

    scene->markInstanceReadyForBlasBuild(instance);

    toFreeList.freeAll();
}

Instance* Chunk::getInstance() const
{
    return instance;
}

// y changes fastest, then x, then z
//
// for loops should be written like this:
// for (uint z = 0; z < CHUNK_SIZE_Z; ++z)
// {
//     for (uint x = 0; x < CHUNK_SIZE_X; ++x)
//     {
//         for (uint y = 0; y < CHUNK_SIZE_Y; ++y)
//         {
//             // do stuff here
uint32_t Chunk::blockPosToIdx(glm::uvec3 blockPos)
{
    return blockPos.y
		 + blockPos.x * CHUNK_SIZE_Y
		 + blockPos.z * CHUNK_SIZE_XZ * CHUNK_SIZE_Y;
}
