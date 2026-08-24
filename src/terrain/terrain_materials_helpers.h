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
#include <string>
#include <vector>

namespace TerrainMaterials
{

inline constexpr bool DEBUG_EXPORT_MIPMAPS = false;

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

static bool tileHasTransparency(const std::vector<uint8_t>& mip, uint32_t tileSize)
{
    for (uint32_t y = 0; y < tileSize; ++y)
    {
        for (uint32_t x = 0; x < tileSize; ++x)
        {
            if (mip[texelIdx(x, y, tileSize) + 3] < 255)
            {
                return true;
            }
        }
    }
    return false;
}

// The aux map's g channel is the biome tint mask (see MATERIAL_FLAG_PACKED_AUX)
static bool tileHasBiomeTintMask(const std::vector<uint8_t>& mip, uint32_t tileSize)
{
    for (uint32_t y = 0; y < tileSize; ++y)
    {
        for (uint32_t x = 0; x < tileSize; ++x)
        {
            if (mip[texelIdx(x, y, tileSize) + 1] > 0)
            {
                return true;
            }
        }
    }
    return false;
}

static float computeOpaqueFraction(const std::vector<uint8_t>& mip, uint32_t tileSize)
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
            opaqueCount += mip[texelIdx(x, y, tileSize) + 3] > 0 ? 1u : 0u;
        }
    }
    return static_cast<float>(opaqueCount) / static_cast<float>(texelCount);
}

static void downsample2x2(
    const std::vector<uint8_t>& src, std::vector<uint8_t>& dst, uint32_t dstTileSize,
    const bool premultiplyAlpha, const bool srgbTransfer)
{
    constexpr float alphaEpsilon = 1e-6f;
    const uint32_t srcTileSize = dstTileSize * 2;
    const auto decode = [srgbTransfer](uint8_t v) { return srgbTransfer ? linearize(v) : v / 255.f; };
    const auto encode = [srgbTransfer](float v)
    {
        return srgbTransfer ? srgbEncode(v) : static_cast<uint8_t>(std::clamp(v * 255.f + 0.5f, 0.f, 255.f));
    };
    for (uint32_t y = 0; y < dstTileSize; ++y)
    {
        for (uint32_t x = 0; x < dstTileSize; ++x)
        {
            const uint32_t sx = x * 2;
            const uint32_t sy = y * 2;
            const uint8_t* p00 = src.data() + texelIdx(sx, sy, srcTileSize);
            const uint8_t* p10 = src.data() + texelIdx(sx + 1, sy, srcTileSize);
            const uint8_t* p01 = src.data() + texelIdx(sx, sy + 1, srcTileSize);
            const uint8_t* p11 = src.data() + texelIdx(sx + 1, sy + 1, srcTileSize);
            uint8_t* out = dst.data() + texelIdx(x, y, dstTileSize);
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

static void quantizeAlphaToCoverage(std::vector<uint8_t>& mip, uint32_t tileSize, float sourceCoverage)
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
        return mip[texelIdx(x, y, tileSize) + 3];
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
        mip[texelIdx(x, y, tileSize) + 3] = rank < targetOpaque ? 255 : 0;
    }
}

// Makes a cutout tile's lower mips carry color only: every texel becomes fully opaque, with
// uncovered texels taking the average color of their covered neighbors. Used when OMMs perform
// the alpha test (always against mip 0), so the shading alpha must not cut coverage a second
// time. Hits can only land over opaque mip-0 texels, so the dilated colors are defensive only.
static void opaquifyCutoutMip(std::vector<uint8_t>& mip, uint32_t tileSize)
{
    const uint32_t texelCount = tileSize * tileSize;
    const auto texelAt = [&](uint32_t i) { return texelIdx(i % tileSize, i / tileSize, tileSize); };

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

// Writes a tile's mip chain as a horizontal strip PNG to the Downloads folder
static void debugExportMipmaps(const std::string& fileName, const std::vector<std::vector<uint8_t>>& mipData)
{
    namespace fs = std::filesystem;

    uint32_t atlasWidth = 0;
    for (uint32_t m = 0; m < mipData.size(); ++m)
    {
        atlasWidth += std::max(1u, TERRAIN_TILE_SIZE >> m);
    }

    std::vector<uint8_t> atlasData(static_cast<size_t>(atlasWidth) * TERRAIN_TILE_SIZE * 4, 0);
    uint32_t xOffset = 0;
    for (uint32_t m = 0; m < mipData.size(); ++m)
    {
        const uint32_t mipSize = std::max(1u, TERRAIN_TILE_SIZE >> m);
        for (uint32_t y = 0; y < mipSize; ++y)
        {
            uint8_t* dstRow = atlasData.data() + (static_cast<size_t>(y) * atlasWidth + xOffset) * 4;
            const uint8_t* srcRow = mipData[m].data() + static_cast<size_t>(y) * mipSize * 4;
            std::memcpy(dstRow, srcRow, static_cast<size_t>(mipSize) * 4);
        }
        xOffset += mipSize;
    }

    fs::path downloadsPath = fs::current_path();
    PWSTR downloadsPathWide = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &downloadsPathWide)))
    {
        downloadsPath = fs::path(downloadsPathWide);
        CoTaskMemFree(downloadsPathWide);
    }

    const fs::path debugOutputPath = downloadsPath / (fileName + "_mipmap.png");
    const int writeResult = stbi_write_png(
        debugOutputPath.generic_string().c_str(),
        static_cast<int>(atlasWidth),
        static_cast<int>(TERRAIN_TILE_SIZE),
        4,
        atlasData.data(),
        static_cast<int>(atlasWidth * 4));
    if (writeResult == 0)
    {
        Logger::logError("Failed to write mip debug texture to: %s", debugOutputPath.generic_string().c_str());
    }
}

struct LoadTextureOptions
{
    bool sRGB = true;
    // One bool per texture array slice: whether the tile has any biome tint mask coverage at
    // mip 0. Only meaningful for the aux maps.
    std::vector<bool>* outSliceHasBiomeTintMask = nullptr;
    // Mip 0 alpha channel per slice, one byte per texel
    std::vector<std::vector<uint8_t>>* outAlphaChannels = nullptr;
    // Replaces each slice's loaded alpha before mip generation, so a texture co-registered
    // with a cutout texture inherits its coverage weighting.
    // See knowledge/scene/materials_textures.md.
    const std::vector<std::vector<uint8_t>>* alphaOverrides = nullptr;
    // When OMMs perform the cutout alpha test, lower mips carry color only: skip the
    // coverage-preserving alpha quantization and make cutout tiles' mips fully opaque.
    // See knowledge/terrain/terrain_omm.md.
    bool useOpaqueCutoutMips = false;
    // Missing files load as zero-filled tiles instead of failing the whole array; aux maps
    // exist only for the textures with emissive/tint data
    bool missingFilesAreZero = false;
};

// Loads one TERRAIN_TILE_SIZE^2 PNG per texture name from assets/blocks/textures/ into a
// texture array whose slice indices match the given order (see Blocks::getTextureNames())
static uint32_t loadBlockTextureArray(Scene* scene,
                                      const std::vector<std::string>& textureNames,
                                      const std::string& fileNameSuffix,
                                      const LoadTextureOptions& options = {})
{
    namespace fs = std::filesystem;

    const fs::path texturesDir = fs::path(TARGET_FILE_DIR) / fs::path("assets/blocks/textures/");

    constexpr uint32_t numMips = 5;
    static_assert(TERRAIN_TILE_SIZE >> (numMips - 1) == 1);
    constexpr size_t texelCount = static_cast<size_t>(TERRAIN_TILE_SIZE) * TERRAIN_TILE_SIZE;

    const uint32_t numSlices = static_cast<uint32_t>(textureNames.size());
    std::vector<std::vector<std::vector<uint8_t>>> sliceMipData(numSlices);

    if (options.outSliceHasBiomeTintMask != nullptr)
    {
        options.outSliceHasBiomeTintMask->resize(numSlices);
    }
    if (options.outAlphaChannels != nullptr)
    {
        options.outAlphaChannels->resize(numSlices);
    }

    for (uint32_t slice = 0; slice < numSlices; ++slice)
    {
        const std::string fileName = textureNames[slice] + fileNameSuffix;
        const fs::path fullPath = texturesDir / (fileName + ".png");

        std::vector<std::vector<uint8_t>>& mipData = sliceMipData[slice];
        mipData.resize(numMips);
        mipData[0].resize(texelCount * 4, 0);

        if (fs::exists(fullPath))
        {
            int width = 0;
            int height = 0;
            int channels = 0;
            unsigned char* data = stbi_load(fullPath.generic_string().c_str(), &width, &height, &channels, 4);
            if (data == nullptr)
            {
                Logger::logError("Failed to load texture from: %s", fullPath.generic_string().c_str());
                return TEXTURE_ID_INVALID;
            }

            ASSERT(width == TERRAIN_TILE_SIZE && height == TERRAIN_TILE_SIZE);
            std::memcpy(mipData[0].data(), data, mipData[0].size());
            stbi_image_free(data);
        }
        else if (!options.missingFilesAreZero)
        {
            Logger::logError("Failed to load texture from: %s", fullPath.generic_string().c_str());
            return TEXTURE_ID_INVALID;
        }

        if (options.alphaOverrides != nullptr)
        {
            const std::vector<uint8_t>& alphaOverride = (*options.alphaOverrides)[slice];
            ASSERT(alphaOverride.size() == texelCount);
            for (size_t i = 0; i < texelCount; ++i)
            {
                mipData[0][i * 4 + 3] = alphaOverride[i];
            }
        }

        if (options.outAlphaChannels != nullptr)
        {
            std::vector<uint8_t>& outAlpha = (*options.outAlphaChannels)[slice];
            outAlpha.resize(texelCount);
            for (size_t i = 0; i < texelCount; ++i)
            {
                outAlpha[i] = mipData[0][i * 4 + 3];
            }
        }

        if (options.outSliceHasBiomeTintMask != nullptr)
        {
            (*options.outSliceHasBiomeTintMask)[slice] = tileHasBiomeTintMask(mipData[0], TERRAIN_TILE_SIZE);
        }

        const bool hasTransparency = tileHasTransparency(mipData[0], TERRAIN_TILE_SIZE);

        for (uint32_t m = 1; m < numMips; ++m)
        {
            const uint32_t dstTileSize = TERRAIN_TILE_SIZE >> m;
            mipData[m].resize(static_cast<size_t>(dstTileSize) * dstTileSize * 4);
            downsample2x2(mipData[m - 1], mipData[m], dstTileSize, hasTransparency /*premultiplyAlpha*/,
                          options.sRGB /*srgbTransfer*/);
            if (hasTransparency && !options.useOpaqueCutoutMips)
            {
                const float sourceCoverage = computeOpaqueFraction(mipData[m - 1], TERRAIN_TILE_SIZE >> (m - 1));
                quantizeAlphaToCoverage(mipData[m], dstTileSize, sourceCoverage);
            }
        }

        // Opaquify only after the full cascade: the downsamples above weight colors by the
        // previous mip's fractional alpha, which is the tile's true mip-0 coverage.
        if (hasTransparency && options.useOpaqueCutoutMips)
        {
            for (uint32_t m = 1; m < numMips; ++m)
            {
                opaquifyCutoutMip(mipData[m], TERRAIN_TILE_SIZE >> m);
            }
        }

        if constexpr (DEBUG_EXPORT_MIPMAPS)
        {
            debugExportMipmaps(fileName, mipData);
        }
    }

    return scene->addTextureArray(std::move(sliceMipData), TERRAIN_TILE_SIZE, TERRAIN_TILE_SIZE,
                                  options.sRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM);
}

} // namespace TerrainMaterials
