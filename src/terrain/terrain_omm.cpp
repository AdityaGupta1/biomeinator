// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "terrain_omm.h"

#include "debug.h"
#include "rendering/buffer/acs_helper.h"
#include "rendering/buffer/to_free_list.h"

#include <algorithm>
#include <bit>

namespace TerrainOmm
{

static constexpr uint32_t NUM_TRIS_PER_QUAD = 2;

static uint32_t ommSubdivisionLevel = 0;

static std::vector<int32_t> sliceCutoutIdxs; // -1 for slices with no transparency
static std::vector<uint8_t> ommData;         // OC1 2-state bitmasks, one per cutout-slice triangle
static std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_DESC> ommDescs;

static bool baked = false;
static bool buildPending = false;
static ManagedBufferSection ommArraySection{};

// The following three functions decode a micro-triangle's index along the OC1 space-filling
// curve into discrete barycentric coordinates (reference implementation shared by the DXR
// and Vulkan opacity micromap specs).

static uint32_t extractEvenBits(uint32_t x)
{
    x &= 0x55555555u;
    x = (x | (x >> 1)) & 0x33333333u;
    x = (x | (x >> 2)) & 0x0f0f0f0fu;
    x = (x | (x >> 4)) & 0x00ff00ffu;
    x = (x | (x >> 8)) & 0x0000ffffu;
    return x;
}

static uint32_t prefixEor(uint32_t x)
{
    x ^= x >> 1;
    x ^= x >> 2;
    x ^= x >> 4;
    x ^= x >> 8;
    return x;
}

static void index2dbary(uint32_t index, uint32_t& u, uint32_t& v, uint32_t& w)
{
    const uint32_t b0 = extractEvenBits(index);
    const uint32_t b1 = extractEvenBits(index >> 1);

    const uint32_t fx = prefixEor(b0);
    const uint32_t fy = prefixEor(b0 & ~b1);

    const uint32_t t = fy ^ b1;

    u = (fx & ~t) | (b0 & ~t) | (~b0 & ~fx & t);
    v = fy ^ b0;
    w = (~fx & ~t) | (b0 & ~t) | (~b0 & fx & t);
}

// Barycentric-space centroid of the micro-triangle at the given curve index
static void microTriCentroid(const uint32_t index, const uint32_t level, float& outU, float& outV)
{
    uint32_t iu, iv, iw;
    index2dbary(index, iu, iv, iw);

    const uint32_t mask = (1u << level) - 1u;
    iu &= mask;
    iv &= mask;
    iw &= mask;

    const bool upright = (((iu & 1u) ^ (iv & 1u) ^ (iw & 1u)) != 0);
    if (!upright)
    {
        ++iu;
        ++iv;
    }

    // Corners are (u, v), (u + d, v), (u, v + d) with d negated for inverted micro-triangles
    const float levelScale = 1.f / static_cast<float>(1u << level);
    const float d = upright ? levelScale : -levelScale;
    outU = static_cast<float>(iu) * levelScale + d / 3.f;
    outV = static_cast<float>(iv) * levelScale + d / 3.f;
}

void bake(const std::vector<uint8_t>& mip0Alpha, const uint32_t textureSize, const uint32_t tileSize)
{
    ASSERT(!baked);
    ASSERT(std::has_single_bit(tileSize));
    ASSERT(mip0Alpha.size() == static_cast<size_t>(textureSize) * textureSize);

    ommSubdivisionLevel = static_cast<uint32_t>(std::countr_zero(tileSize));
    const uint32_t numMicroTrisPerOmm = tileSize * tileSize; // 4^level
    const uint32_t maskSizeBytes = numMicroTrisPerOmm / 8;

    // Texture UVs of each quad triangle's corners; must match chunk.cpp's uvOffsets and its
    // (0,1,2),(0,2,3) index split
    static constexpr float triCornerUvs[NUM_TRIS_PER_QUAD][3][2] = {
        { { 1.f, 0.f }, { 0.f, 0.f }, { 0.f, 1.f } },
        { { 1.f, 0.f }, { 0.f, 1.f }, { 1.f, 1.f } },
    };

    const uint32_t tilesPerAxis = textureSize / tileSize;
    sliceCutoutIdxs.assign(static_cast<size_t>(tilesPerAxis) * tilesPerAxis, -1);

    for (uint32_t tileY = 0; tileY < tilesPerAxis; ++tileY)
    {
        for (uint32_t tileX = 0; tileX < tilesPerAxis; ++tileX)
        {
            const auto alphaAt = [&](uint32_t texelX, uint32_t texelY) {
                return mip0Alpha[static_cast<size_t>(tileY * tileSize + texelY) * textureSize + tileX * tileSize + texelX];
            };

            uint32_t numOpaqueTexels = 0;
            bool hasTransparency = false;
            for (uint32_t y = 0; y < tileSize; ++y)
            {
                for (uint32_t x = 0; x < tileSize; ++x)
                {
                    const uint8_t alpha = alphaAt(x, y);
                    // 2-state OMMs reproduce the alpha test exactly only if cutout alpha is binary
                    ASSERT(alpha == 0 || alpha == 255);
                    numOpaqueTexels += (alpha != 0) ? 1u : 0u;
                    hasTransparency |= (alpha < 255);
                }
            }
            if (!hasTransparency)
            {
                continue;
            }

            sliceCutoutIdxs[tileY * tilesPerAxis + tileX] = static_cast<int32_t>(ommDescs.size() / NUM_TRIS_PER_QUAD);

            uint32_t numOpaqueMicroTris = 0;
            for (uint32_t triInQuad = 0; triInQuad < NUM_TRIS_PER_QUAD; ++triInQuad)
            {
                const uint32_t maskOffsetBytes = static_cast<uint32_t>(ommData.size());
                ommData.resize(ommData.size() + maskSizeBytes, 0);
                ommDescs.push_back({
                    .ByteOffset = maskOffsetBytes,
                    .SubdivisionLevel = static_cast<UINT>(ommSubdivisionLevel),
                    .Format = D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_2_STATE,
                });

                const auto& cornerUvs = triCornerUvs[triInQuad];
                for (uint32_t microTriIdx = 0; microTriIdx < numMicroTrisPerOmm; ++microTriIdx)
                {
                    float baryU, baryV;
                    microTriCentroid(microTriIdx, ommSubdivisionLevel, baryU, baryV);

                    // Each micro-triangle lies entirely inside one texel, so sampling the
                    // centroid reproduces the point-sampled alpha test exactly
                    const float baryW = 1.f - baryU - baryV;
                    const float texU = cornerUvs[0][0] * baryW + cornerUvs[1][0] * baryU + cornerUvs[2][0] * baryV;
                    const float texV = cornerUvs[0][1] * baryW + cornerUvs[1][1] * baryU + cornerUvs[2][1] * baryV;
                    const uint32_t texelX = std::min(tileSize - 1, static_cast<uint32_t>(texU * tileSize));
                    const uint32_t texelY = std::min(tileSize - 1, static_cast<uint32_t>(texV * tileSize));

                    if (alphaAt(texelX, texelY) != 0)
                    {
                        ommData[maskOffsetBytes + microTriIdx / 8] |= 1u << (microTriIdx % 8);
                        ++numOpaqueMicroTris;
                    }
                }
            }

            // The quad's two micromaps together cover each texel with exactly two
            // micro-triangles, so opaque micro-tris must be exactly twice the opaque texels
            ASSERT(numOpaqueMicroTris == numOpaqueTexels * 2);
        }
    }

    baked = true;
    buildPending = !ommDescs.empty();
}

bool isBaked()
{
    return baked;
}

bool sliceHasCutout(const uint32_t sliceIdx)
{
    return baked && sliceIdx < sliceCutoutIdxs.size() && sliceCutoutIdxs[sliceIdx] >= 0;
}

uint16_t getOmmIdx(const uint32_t sliceIdx, const uint32_t triInQuad)
{
    ASSERT(sliceHasCutout(sliceIdx));
    ASSERT(triInQuad < NUM_TRIS_PER_QUAD);
    return static_cast<uint16_t>(sliceCutoutIdxs[sliceIdx] * NUM_TRIS_PER_QUAD + triInQuad);
}

void buildArrayIfPending(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    if (!buildPending)
    {
        return;
    }
    buildPending = false;

    AcsHelper::OmmArrayBuildInputs inputs;
    inputs.host_ommData = &ommData;
    inputs.host_ommDescs = &ommDescs;
    inputs.histogram = {
        .Count = static_cast<UINT>(ommDescs.size()),
        .SubdivisionLevel = static_cast<UINT>(ommSubdivisionLevel),
        .Format = D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_2_STATE,
    };
    inputs.outOmmArray = &ommArraySection;

    AcsHelper::buildOmmArray(cmdList, toFreeList, inputs);
}

void reset()
{
    ommArraySection.free();
    ommData.clear();
    ommDescs.clear();
    sliceCutoutIdxs.clear();
    baked = false;
    buildPending = false;
}

} // namespace TerrainOmm
