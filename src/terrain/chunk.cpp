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

Chunk::Chunk(ivec2 chunkPos)
	: chunkPos(chunkPos)
{}

void Chunk::generateBlocks()
{
    const ivec2 chunkBlockPos_WS = chunkPos * 16;

    for (uint y = 0; y < 16; ++y)
    {
        for (uint x = 0; x < 16; ++x)
        {
            const ivec2 blockPosXY_WS = chunkBlockPos_WS + ivec2(x, y);
            const uint height = uint(64.f + 10.f * (sinf(blockPosXY_WS.x * 0.1f) * cosf(blockPosXY_WS.y * 0.1f)));

            for (uint z = 0; z < height; ++z)
            {
                const ivec3 blockPos_CS = ivec3(x, y, z);
                const ivec3 blockPos_WS = ivec3(blockPosXY_WS, 0) + blockPos_CS;
                this->blocks[blockPosToIdx(blockPos_CS)] = rand1(uvec3(blockPos_WS)) < 0.1f ? Block::LAMP : Block::STONE;
            }

            if (rand1(uvec2(blockPosXY_WS)) < 0.05f)
            {
                this->blocks[blockPosToIdx(ivec3(x, y, height))] = Block::LAMP;
            }
        }
    }
}

void Chunk::createInstance(Scene* scene)
{
    ToFreeList toFreeList{};

    // TODO: will have to revisit this when implementing multithreading
    // could potentially be a case where the instances array is resized while some instances are still being worked on, so their data would be lost
    this->instance = scene->requestNewInstance(toFreeList);


    const ivec2 chunkBlockPos_WS = chunkPos * 16;
    const XMMATRIX transform = XMMatrixTranslation(
        static_cast<float>(chunkBlockPos_WS.x),
        static_cast<float>(chunkBlockPos_WS.y),
        0.0f);
    XMFLOAT3X4 instanceTransform;
    XMStoreFloat3x4(&instanceTransform, transform);

    std::vector<Vertex> verts;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> emissiveTriangleIndices;

    // TODO: build chunk with z-up and use transform matrix to rotate it to y-up
    constexpr vec3 faceNormals[6] = {
        vec3(1.0f, 0.0f, 0.0f),   // +x
        vec3(-1.0f, 0.0f, 0.0f),  // -x
        vec3(0.0f, 0.0f, 1.0f),   // +y (chunk) -> +z (world)
        vec3(0.0f, 0.0f, -1.0f),  // -y (chunk) -> -z (world)
        vec3(0.0f, 1.0f, 0.0f),   // +z (chunk) -> +y (world)
        vec3(0.0f, -1.0f, 0.0f),  // -z (chunk) -> -y (world)
    };

    // TODO: build chunk with z-up and use transform matrix to rotate it to y-up
    constexpr ivec3 faceOffsets[6] = {
        ivec3(1, 0, 0),   // +x
        ivec3(-1, 0, 0),  // -x
        ivec3(0, 1, 0),   // +y
        ivec3(0, -1, 0),  // -y
        ivec3(0, 0, 1),   // +z
        ivec3(0, 0, -1),  // -z
    };

    // TODO: extract this to helper function
    auto isBlockAir = [&](int x, int y, int z) -> bool {
        if (x < 0 || x >= static_cast<int>(CHUNK_SIZE_X) ||
            y < 0 || y >= static_cast<int>(CHUNK_SIZE_Y) ||
            z < 0 || z >= static_cast<int>(CHUNK_SIZE_Z))
        {
            // TODO: account for blocks in neighboring chunks
            return true;
        }
        return blocks[blockPosToIdx(uvec3(x, y, z))] == Block::AIR;
    };

    const float texSizeX = static_cast<float>(DEFAULT_TEX_SIZE_X);
    const float texSizeY = static_cast<float>(DEFAULT_TEX_SIZE_Y);

    for (uint y = 0; y < CHUNK_SIZE_Y; ++y)
    {
        for (uint x = 0; x < CHUNK_SIZE_X; ++x)
        {
            for (uint z = 0; z < CHUNK_SIZE_Z; ++z)
            {
                const uvec3 blockPos_CS(x, y, z);
                const Block block = blocks[blockPosToIdx(blockPos_CS)];
                if (block == Block::AIR)
                {
                    continue;
                }

                const BlockData& blockData = Blocks::getBlockData(block);
                const bool isEmissive = blockData.emitsLight;

                for (uint faceIdx = 0; faceIdx < 6; ++faceIdx)
                {
                    const ivec3 neighborOffset = faceOffsets[faceIdx];
                    const ivec3 neighborPos(
                        static_cast<int>(x) + neighborOffset.x,
                        static_cast<int>(y) + neighborOffset.y,
                        static_cast<int>(z) + neighborOffset.z);

                    if (!isBlockAir(neighborPos.x, neighborPos.y, neighborPos.z))
                    {
                        continue;
                    }

                    const vec3 normal = faceNormals[faceIdx];
                    const vec3 basePos(static_cast<float>(x), static_cast<float>(z), static_cast<float>(y));

                    vec3 faceVerts[4];
                    switch (faceIdx)
                    {
                        // TODO: build chunk with z-up and use transform matrix to rotate it to y-up
                        // TODO: use an array instead of a switch statement
                        case 0: // +x
                            faceVerts[0] = basePos + vec3(1.0f, 0.0f, 0.0f);
                            faceVerts[1] = basePos + vec3(1.0f, 1.0f, 0.0f);
                            faceVerts[2] = basePos + vec3(1.0f, 1.0f, 1.0f);
                            faceVerts[3] = basePos + vec3(1.0f, 0.0f, 1.0f);
                            break;
                        case 1: // -x
                            faceVerts[0] = basePos + vec3(0.0f, 0.0f, 0.0f);
                            faceVerts[1] = basePos + vec3(0.0f, 0.0f, 1.0f);
                            faceVerts[2] = basePos + vec3(0.0f, 1.0f, 1.0f);
                            faceVerts[3] = basePos + vec3(0.0f, 1.0f, 0.0f);
                            break;
                        case 2: // +y (chunk space) -> +z (world space)
                            faceVerts[0] = basePos + vec3(0.0f, 0.0f, 1.0f);
                            faceVerts[1] = basePos + vec3(0.0f, 1.0f, 1.0f);
                            faceVerts[2] = basePos + vec3(1.0f, 1.0f, 1.0f);
                            faceVerts[3] = basePos + vec3(1.0f, 0.0f, 1.0f);
                            break;
                        case 3: // -y (chunk space) -> -z (world space)
                            faceVerts[0] = basePos + vec3(0.0f, 0.0f, 0.0f);
                            faceVerts[1] = basePos + vec3(1.0f, 0.0f, 0.0f);
                            faceVerts[2] = basePos + vec3(1.0f, 1.0f, 0.0f);
                            faceVerts[3] = basePos + vec3(0.0f, 1.0f, 0.0f);
                            break;
                        case 4: // +z (chunk space) -> +y (world space)
                            faceVerts[0] = basePos + vec3(0.0f, 1.0f, 0.0f);
                            faceVerts[1] = basePos + vec3(1.0f, 1.0f, 0.0f);
                            faceVerts[2] = basePos + vec3(1.0f, 1.0f, 1.0f);
                            faceVerts[3] = basePos + vec3(0.0f, 1.0f, 1.0f);
                            break;
                        case 5: // -z (chunk space) -> -y (world space)
                            faceVerts[0] = basePos + vec3(0.0f, 0.0f, 0.0f);
                            faceVerts[1] = basePos + vec3(0.0f, 0.0f, 1.0f);
                            faceVerts[2] = basePos + vec3(1.0f, 0.0f, 1.0f);
                            faceVerts[3] = basePos + vec3(1.0f, 0.0f, 0.0f);
                            break;
                    }

                    const uvec2 baseTexCoords = blockData.texCoords;
                    // TODO: might need to rotate these
                    const uvec2 uvOffsets[4] = {
                        uvec2(0, 0),
                        uvec2(1, 0),
                        uvec2(1, 1),
                        uvec2(0, 1),
                    };

                    const uint32_t baseVertIdx = static_cast<uint32_t>(verts.size());
                    for (uint i = 0; i < 4; ++i)
                    {
                        const vec2 uv = (vec2(baseTexCoords + uvOffsets[i])) / vec2(texSizeX, texSizeY);
                        verts.push_back({
                            // TODO: helper functions for converting glm to DirectX
                            { faceVerts[i].x, faceVerts[i].y, faceVerts[i].z },
                            { normal.x, normal.y, normal.z },
                            { uv.x, uv.y },
                        });
                    }

                    const uint32_t triangleIdx = static_cast<uint32_t>(indices.size() / 3);

                    indices.push_back(baseVertIdx + 0);
                    indices.push_back(baseVertIdx + 1);
                    indices.push_back(baseVertIdx + 2);
                    indices.push_back(baseVertIdx + 0);
                    indices.push_back(baseVertIdx + 2);
                    indices.push_back(baseVertIdx + 3);

                    if (isEmissive)
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

// z changes fastest, then x, then y
//
// for loops should be written like this:
// for (uint y = 0; y < CHUNK_SIZE_Y; ++y)
// {
//     for (uint x = 0; x < CHUNK_SIZE_X; ++x)
//     {
//         for (uint z = 0; z < CHUNK_SIZE_Z; ++z)
//         {
//             // ...
uint32_t Chunk::blockPosToIdx(glm::uvec3 blockPos)
{
    return blockPos.z
		 + blockPos.x * CHUNK_SIZE_Z
		 + blockPos.y * CHUNK_SIZE_X * CHUNK_SIZE_Z;
}
