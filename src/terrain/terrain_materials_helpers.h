// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "debug.h"
#include "logger.h"
#include "rendering/common/common_structs.h"
#include "scene/scene.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <shlobj.h>
#include <stb_image.h>
#include <stb_image_write.h>
#include <vector>

namespace TerrainMaterials
{

inline constexpr bool DEBUG_EXPORT_MIPMAPS = false;

inline constexpr uint32_t TERRAIN_TEXTURE_SIZE = 512;
inline constexpr uint32_t TERRAIN_TILE_SIZE = 16;

static float linearize(uint8_t srgb)
{
    const float c = srgb / 255.f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

static uint8_t srgbEncode(float linear)
{
    const float c = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(c * 255.f + 0.5f, 0.f, 255.f));
}

static size_t texelIdx(uint32_t x, uint32_t y, uint32_t width)
{
    return (static_cast<size_t>(y) * width + x) * 4;
}

static bool tileHasTransparency(const std::vector<uint8_t>& mip, uint32_t width, uint32_t tileX, uint32_t tileY, uint32_t tileSize)
{
    for (uint32_t y = 0; y < tileSize; ++y)
    {
        for (uint32_t x = 0; x < tileSize; ++x)
        {
            if (mip[texelIdx(tileX + x, tileY + y, width) + 3] < 255)
            {
                return true;
            }
        }
    }
    return false;
}

// The aux map's g channel is the biome tint mask (see MATERIAL_FLAG_PACKED_AUX)
static bool tileHasBiomeTintMask(const std::vector<uint8_t>& mip, uint32_t width, uint32_t tileX, uint32_t tileY, uint32_t tileSize)
{
    for (uint32_t y = 0; y < tileSize; ++y)
    {
        for (uint32_t x = 0; x < tileSize; ++x)
        {
            if (mip[texelIdx(tileX + x, tileY + y, width) + 1] > 0)
            {
                return true;
            }
        }
    }
    return false;
}

static float computeOpaqueFractionTile(const std::vector<uint8_t>& mip, uint32_t width, uint32_t tileX, uint32_t tileY, uint32_t tileSize)
{
    const uint32_t texelCount = tileSize * tileSize;
    if (texelCount == 0)
    {
        return 0.f;
    }

    uint32_t opaqueCount = 0;
    for (uint32_t y = 0; y < tileSize; ++y)
    {
        for (uint32_t x = 0; x < tileSize; ++x)
        {
            opaqueCount += mip[texelIdx(tileX + x, tileY + y, width) + 3] > 0 ? 1u : 0u;
        }
    }
    return static_cast<float>(opaqueCount) / static_cast<float>(texelCount);
}

static void downsample2x2Tile(
    const std::vector<uint8_t>& src, uint32_t srcWidth, std::vector<uint8_t>& dst, uint32_t dstWidth, uint32_t srcTileX,
    uint32_t srcTileY, uint32_t dstTileX, uint32_t dstTileY, uint32_t dstTileSize,
    const bool premultiplyAlpha, const bool srgbTransfer)
{
    constexpr float alphaEpsilon = 1e-6f;
    const auto decode = [srgbTransfer](uint8_t v) { return srgbTransfer ? linearize(v) : v / 255.f; };
    const auto encode = [srgbTransfer](float v)
    {
        return srgbTransfer ? srgbEncode(v) : static_cast<uint8_t>(std::clamp(v * 255.f + 0.5f, 0.f, 255.f));
    };
    for (uint32_t y = 0; y < dstTileSize; ++y)
    {
        for (uint32_t x = 0; x < dstTileSize; ++x)
        {
            const uint32_t sx = srcTileX + x * 2;
            const uint32_t sy = srcTileY + y * 2;
            const uint8_t* p00 = src.data() + texelIdx(sx, sy, srcWidth);
            const uint8_t* p10 = src.data() + texelIdx(sx + 1, sy, srcWidth);
            const uint8_t* p01 = src.data() + texelIdx(sx, sy + 1, srcWidth);
            const uint8_t* p11 = src.data() + texelIdx(sx + 1, sy + 1, srcWidth);
            uint8_t* out = dst.data() + texelIdx(dstTileX + x, dstTileY + y, dstWidth);
            const float a00 = p00[3] / 255.f;
            const float a10 = p10[3] / 255.f;
            const float a01 = p01[3] / 255.f;
            const float a11 = p11[3] / 255.f;
            const float avgA = (a00 + a10 + a01 + a11) * 0.25f;

            for (uint32_t ch = 0; ch < 3; ++ch)
            {
                if (premultiplyAlpha)
                {
                    const float avgPremultiplied = (decode(p00[ch]) * a00 + decode(p10[ch]) * a10
                                                   + decode(p01[ch]) * a01 + decode(p11[ch]) * a11)
                        * 0.25f;
                    const float avg = avgA > alphaEpsilon ? (avgPremultiplied / avgA) : 0.f;
                    out[ch] = encode(avg);
                    continue;
                }

                const float avg = (decode(p00[ch]) + decode(p10[ch]) + decode(p01[ch]) + decode(p11[ch])) * 0.25f;
                out[ch] = encode(avg);
            }
            if (premultiplyAlpha)
            {
                out[3] = static_cast<uint8_t>(std::clamp(avgA * 255.f + 0.5f, 0.f, 255.f));
            }
            else
            {
                out[3] = 255;
            }
        }
    }
}

static void quantizeAlphaToCoverageTile(
    std::vector<uint8_t>& mip, uint32_t width, uint32_t tileX, uint32_t tileY, uint32_t tileSize, float sourceCoverage)
{
    const uint32_t texelCount = tileSize * tileSize;
    const uint32_t targetOpaque = static_cast<uint32_t>(std::clamp(
        static_cast<int64_t>(std::llround(sourceCoverage * static_cast<float>(texelCount))),
        int64_t(0),
        static_cast<int64_t>(texelCount)));

    std::vector<uint32_t> order(texelCount);
    std::iota(order.begin(), order.end(), 0u);
    const auto alphaAt = [&](uint32_t i) {
        const uint32_t x = i % tileSize;
        const uint32_t y = i / tileSize;
        return mip[texelIdx(tileX + x, tileY + y, width) + 3];
    };
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const uint8_t alphaA = alphaAt(a);
        const uint8_t alphaB = alphaAt(b);
        return alphaA == alphaB ? a < b : alphaA > alphaB;
    });

    for (uint32_t rank = 0; rank < texelCount; ++rank)
    {
        const uint32_t i = order[rank];
        const uint32_t x = i % tileSize;
        const uint32_t y = i / tileSize;
        mip[texelIdx(tileX + x, tileY + y, width) + 3] = rank < targetOpaque ? 255 : 0;
    }
}

// Makes a cutout tile's lower mips carry color only: every texel becomes fully opaque, with
// uncovered texels taking the average color of their covered neighbors. Used when OMMs perform
// the alpha test (always against mip 0), so the shading alpha must not cut coverage a second
// time. Hits can only land over opaque mip-0 texels, so the dilated colors are defensive only.
static void opaquifyCutoutMipTile(
    std::vector<uint8_t>& mip, uint32_t width, uint32_t tileX, uint32_t tileY, uint32_t tileSize)
{
    const uint32_t texelCount = tileSize * tileSize;
    const auto texelAt = [&](uint32_t i) { return texelIdx(tileX + i % tileSize, tileY + i / tileSize, width); };

    std::vector<bool> covered(texelCount);
    uint32_t coveredCount = 0;
    for (uint32_t i = 0; i < texelCount; ++i)
    {
        covered[i] = mip[texelAt(i) + 3] > 0;
        coveredCount += covered[i] ? 1u : 0u;
    }

    while (coveredCount > 0 && coveredCount < texelCount)
    {
        const std::vector<bool> prevCovered = covered;
        for (uint32_t i = 0; i < texelCount; ++i)
        {
            if (prevCovered[i])
            {
                continue;
            }

            const int32_t x = static_cast<int32_t>(i % tileSize);
            const int32_t y = static_cast<int32_t>(i / tileSize);
            uint32_t sum[3] = { 0, 0, 0 };
            uint32_t numNeighbors = 0;
            constexpr int32_t offsets[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
            for (const auto& offset : offsets)
            {
                const int32_t nx = x + offset[0];
                const int32_t ny = y + offset[1];
                if (nx < 0 || ny < 0 || nx >= static_cast<int32_t>(tileSize) || ny >= static_cast<int32_t>(tileSize))
                {
                    continue;
                }

                const uint32_t neighborIdx = static_cast<uint32_t>(ny) * tileSize + static_cast<uint32_t>(nx);
                if (!prevCovered[neighborIdx])
                {
                    continue;
                }

                const size_t neighborTexel = texelAt(neighborIdx);
                for (uint32_t ch = 0; ch < 3; ++ch)
                {
                    sum[ch] += mip[neighborTexel + ch];
                }
                ++numNeighbors;
            }

            if (numNeighbors > 0)
            {
                const size_t texel = texelAt(i);
                for (uint32_t ch = 0; ch < 3; ++ch)
                {
                    mip[texel + ch] = static_cast<uint8_t>(sum[ch] / numNeighbors);
                }
                covered[i] = true;
                ++coveredCount;
            }
        }
    }

    for (uint32_t i = 0; i < texelCount; ++i)
    {
        mip[texelAt(i) + 3] = 255;
    }
}

struct LoadTextureOptions
{
    bool sRGB = true;
    // One bool per tile, indexed like the returned array's slices: whether the tile has any biome
    // tint mask coverage at mip 0. Only meaningful for the aux map.
    std::vector<bool>* outTileHasBiomeTintMask = nullptr;
    // Mip 0 alpha channel, one byte per texel
    std::vector<uint8_t>* outAlphaChannel = nullptr;
    // Replaces the loaded alpha before mip generation, so a texture co-registered with a cutout
    // texture inherits its coverage weighting. See knowledge/scene/materials_textures.md.
    const std::vector<uint8_t>* alphaOverride = nullptr;
    // When OMMs perform the cutout alpha test, lower mips carry color only: skip the
    // coverage-preserving alpha quantization and make cutout tiles' mips fully opaque.
    // See knowledge/terrain/terrain_omm.md.
    bool useOpaqueCutoutMips = false;
};

static uint32_t loadTexture(Scene* scene, const std::filesystem::path& filename, const LoadTextureOptions& options = {})
{
    namespace fs = std::filesystem;

    const fs::path fullPath = fs::path(TARGET_FILE_DIR) / fs::path("assets/textures/") / filename;

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* data = stbi_load(fullPath.generic_string().c_str(), &width, &height, &channels, 4);

    if (data == nullptr)
    {
        Logger::logError("Failed to load texture from: %s", fullPath.generic_string().c_str());
        return TEXTURE_ID_INVALID;
    }

    const uint32_t w0 = static_cast<uint32_t>(width);
    const uint32_t h0 = static_cast<uint32_t>(height);
    constexpr uint32_t textureSize = TERRAIN_TEXTURE_SIZE;
    constexpr uint32_t tileSizeMip0 = TERRAIN_TILE_SIZE;
    constexpr uint32_t numMips = 5;
    // Must match DEFAULT_TEX_NUM_BLOCKS_X in chunk.cpp.
    static_assert(textureSize / tileSizeMip0 == 32);
    ASSERT(w0 == textureSize && h0 == textureSize);

    const size_t texelCount = static_cast<size_t>(w0) * h0;
    std::vector<std::vector<uint8_t>> mipData(numMips);

    // Mip 0: copy raw stb_image data
    mipData[0].resize(texelCount * 4);
    std::memcpy(mipData[0].data(), data, mipData[0].size());
    stbi_image_free(data);

    if (options.alphaOverride != nullptr)
    {
        ASSERT(options.alphaOverride->size() == texelCount);
        for (size_t i = 0; i < texelCount; ++i)
        {
            mipData[0][i * 4 + 3] = (*options.alphaOverride)[i];
        }
    }

    if (options.outAlphaChannel != nullptr)
    {
        options.outAlphaChannel->resize(texelCount);
        for (size_t i = 0; i < texelCount; ++i)
        {
            (*options.outAlphaChannel)[i] = mipData[0][i * 4 + 3];
        }
    }

    for (uint32_t m = 1; m < numMips; ++m)
    {
        const uint32_t wDst = w0 >> m;
        const uint32_t hDst = h0 >> m;
        mipData[m].resize(static_cast<size_t>(wDst) * hDst * 4);
    }

    const uint32_t tilesPerAxis = textureSize / tileSizeMip0;
    if (options.outTileHasBiomeTintMask != nullptr)
    {
        options.outTileHasBiomeTintMask->resize(static_cast<size_t>(tilesPerAxis) * tilesPerAxis);
    }
    for (uint32_t tileY = 0; tileY < tilesPerAxis; ++tileY)
    {
        for (uint32_t tileX = 0; tileX < tilesPerAxis; ++tileX)
        {
            const uint32_t mip0TileX = tileX * tileSizeMip0;
            const uint32_t mip0TileY = tileY * tileSizeMip0;
            if (options.outTileHasBiomeTintMask != nullptr)
            {
                (*options.outTileHasBiomeTintMask)[tileY * tilesPerAxis + tileX] =
                    tileHasBiomeTintMask(mipData[0], w0, mip0TileX, mip0TileY, tileSizeMip0);
            }
            const bool hasTransparency = tileHasTransparency(mipData[0], w0, mip0TileX, mip0TileY, tileSizeMip0);

            for (uint32_t m = 1; m < numMips; ++m)
            {
                const uint32_t srcWidth = w0 >> (m - 1);
                const uint32_t dstWidth = w0 >> m;
                const uint32_t srcTileSize = tileSizeMip0 >> (m - 1);
                const uint32_t dstTileSize = tileSizeMip0 >> m;
                const uint32_t srcTileX = tileX * srcTileSize;
                const uint32_t srcTileY = tileY * srcTileSize;
                const uint32_t dstTileX = tileX * dstTileSize;
                const uint32_t dstTileY = tileY * dstTileSize;

                downsample2x2Tile(mipData[m - 1], srcWidth, mipData[m], dstWidth, srcTileX, srcTileY, dstTileX,
                                  dstTileY, dstTileSize, hasTransparency /*premultiplyAlpha*/,
                                  options.sRGB /*srgbTransfer*/);
                if (hasTransparency && !options.useOpaqueCutoutMips)
                {
                    const float sourceCoverage =
                        computeOpaqueFractionTile(mipData[m - 1], srcWidth, srcTileX, srcTileY, srcTileSize);
                    quantizeAlphaToCoverageTile(mipData[m], dstWidth, dstTileX, dstTileY, dstTileSize, sourceCoverage);
                }
            }

            // Opaquify only after the full cascade: the downsamples above weight colors by the
            // previous mip's fractional alpha, which is the tile's true mip-0 coverage.
            if (hasTransparency && options.useOpaqueCutoutMips)
            {
                for (uint32_t m = 1; m < numMips; ++m)
                {
                    const uint32_t mipTileSize = tileSizeMip0 >> m;
                    opaquifyCutoutMipTile(mipData[m], w0 >> m, tileX * mipTileSize, tileY * mipTileSize, mipTileSize);
                }
            }
        }
    }

    if constexpr (DEBUG_EXPORT_MIPMAPS)
    {
        std::vector<uint32_t> mipWidths(numMips);
        std::vector<uint32_t> mipHeights(numMips);
        uint32_t atlasWidth = 0;
        uint32_t atlasHeight = 0;
        for (uint32_t m = 0; m < numMips; ++m)
        {
            const uint32_t w = std::max(1u, w0 >> m);
            const uint32_t h = std::max(1u, h0 >> m);
            mipWidths[m] = w;
            mipHeights[m] = h;
            atlasWidth += w;
            atlasHeight = std::max(atlasHeight, h);
        }

        std::vector<uint8_t> atlasData(static_cast<size_t>(atlasWidth) * atlasHeight * 4, 0);
        uint32_t xOffset = 0;
        for (uint32_t m = 0; m < numMips; ++m)
        {
            const uint32_t w = mipWidths[m];
            const uint32_t h = mipHeights[m];
            const uint8_t* src = mipData[m].data();
            for (uint32_t y = 0; y < h; ++y)
            {
                uint8_t* dstRow = atlasData.data() + (static_cast<size_t>(y) * atlasWidth + xOffset) * 4;
                const uint8_t* srcRow = src + static_cast<size_t>(y) * w * 4;
                std::memcpy(dstRow, srcRow, static_cast<size_t>(w) * 4);
            }
            xOffset += w;
        }

        fs::path downloadsPath = fs::current_path();
        PWSTR downloadsPathWide = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &downloadsPathWide)))
        {
            downloadsPath = fs::path(downloadsPathWide);
            CoTaskMemFree(downloadsPathWide);
        }

        const fs::path debugOutputPath = downloadsPath / (filename.stem().string() + "_mipmap.png");
        const int writeResult = stbi_write_png(
            debugOutputPath.generic_string().c_str(),
            static_cast<int>(atlasWidth),
            static_cast<int>(atlasHeight),
            4,
            atlasData.data(),
            static_cast<int>(atlasWidth * 4));
        if (writeResult == 0)
        {
            Logger::logError("Failed to write mip debug texture to: %s", debugOutputPath.generic_string().c_str());
        }
    }

    // Atlas mip chain -> per-tile slices. Order: tileY * tilesPerAxis + tileX (matches chunk.cpp).
    const uint32_t numSlices = tilesPerAxis * tilesPerAxis;
    std::vector<std::vector<std::vector<uint8_t>>> sliceMipData(numSlices);
    for (uint32_t slice = 0; slice < numSlices; ++slice)
    {
        sliceMipData[slice].resize(numMips);
    }

    for (uint32_t m = 0; m < numMips; ++m)
    {
        const uint32_t mipWidth = w0 >> m;
        const uint32_t mipTileSize = std::max(1u, tileSizeMip0 >> m);
        const uint32_t bytesPerTile = mipTileSize * mipTileSize * 4;
        for (uint32_t tileY = 0; tileY < tilesPerAxis; ++tileY)
        {
            for (uint32_t tileX = 0; tileX < tilesPerAxis; ++tileX)
            {
                const uint32_t slice = tileY * tilesPerAxis + tileX;
                std::vector<uint8_t>& dst = sliceMipData[slice][m];
                dst.resize(bytesPerTile);
                const uint32_t srcTileX = tileX * mipTileSize;
                const uint32_t srcTileY = tileY * mipTileSize;
                for (uint32_t y = 0; y < mipTileSize; ++y)
                {
                    const uint8_t* srcRow = mipData[m].data() + texelIdx(srcTileX, srcTileY + y, mipWidth);
                    uint8_t* dstRow = dst.data() + static_cast<size_t>(y) * mipTileSize * 4;
                    std::memcpy(dstRow, srcRow, static_cast<size_t>(mipTileSize) * 4);
                }
            }
        }
    }

    return scene->addTextureArray(std::move(sliceMipData), tileSizeMip0, tileSizeMip0,
                                  options.sRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM);
}

} // namespace TerrainMaterials
