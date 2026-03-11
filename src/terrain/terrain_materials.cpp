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

#include "terrain_materials.h"

#include "logger.h"
#include "rendering/common/common_structs.h"
#include "rendering/buffer/to_free_list.h"
#include "scene/scene.h"

#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

namespace TerrainMaterials
{

static uint32_t loadTexture(Scene* scene, const std::filesystem::path& path);

static void createMaterials(Scene* scene);

void init(Scene* scene)
{
    createMaterials(scene);
}

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

static uint32_t loadTexture(Scene* scene, const std::filesystem::path& filename)
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
    constexpr uint32_t numMips = 5;

    std::vector<std::vector<uint8_t>> mipData(numMips);

    // Mip 0: copy raw stb_image data
    mipData[0].resize(static_cast<size_t>(w0) * h0 * 4);
    std::memcpy(mipData[0].data(), data, mipData[0].size());
    stbi_image_free(data);

    constexpr float alphaTestThreshold = 0.999f;
    constexpr float alphaEpsilon = 1e-6f;
    const uint8_t alphaTestThresholdByte = static_cast<uint8_t>(std::ceil(alphaTestThreshold * 255.f));

    const size_t texelCountMip0 = mipData[0].size() / 4;
    uint32_t numPassingMip0 = 0;
    bool hasTransparency = false;
    for (size_t i = 0; i < texelCountMip0; ++i)
    {
        const uint8_t a = mipData[0][i * 4 + 3];
        hasTransparency |= (a < 255);
        numPassingMip0 += (a >= alphaTestThresholdByte) ? 1u : 0u;
    }

    const float targetAlphaCoverage = texelCountMip0 > 0
        ? static_cast<float>(numPassingMip0) / static_cast<float>(texelCountMip0)
        : 0.f;

    const auto preserveAlphaCoverage = [alphaTestThresholdByte, targetAlphaCoverage](std::vector<uint8_t>& mip) {
        const size_t texelCount = mip.size() / 4;
        if (texelCount == 0)
        {
            return;
        }

        std::array<uint32_t, 256> histogram{};
        for (size_t i = 0; i < texelCount; ++i)
        {
            ++histogram[mip[i * 4 + 3]];
        }

        const uint32_t targetPassing = static_cast<uint32_t>(std::clamp(
            static_cast<int64_t>(std::llround(targetAlphaCoverage * static_cast<float>(texelCount))),
            int64_t(0),
            static_cast<int64_t>(texelCount)));

        uint32_t passing = 0;
        uint32_t bestDiff = ~0u;
        uint8_t sourceThresholdByte = 0;
        for (int t = 255; t >= 0; --t)
        {
            passing += histogram[static_cast<size_t>(t)];
            const uint32_t diff = (passing > targetPassing) ? (passing - targetPassing) : (targetPassing - passing);
            if (diff < bestDiff)
            {
                bestDiff = diff;
                sourceThresholdByte = static_cast<uint8_t>(t);
            }
        }

        if (sourceThresholdByte == alphaTestThresholdByte)
        {
            return;
        }

        const float scale = (255.f - alphaTestThresholdByte)
            / std::max(1.f, 255.f - static_cast<float>(sourceThresholdByte));
        const float bias = alphaTestThresholdByte - scale * static_cast<float>(sourceThresholdByte);

        for (size_t i = 0; i < texelCount; ++i)
        {
            const size_t alphaIdx = i * 4 + 3;
            const float remapped = scale * static_cast<float>(mip[alphaIdx]) + bias;
            mip[alphaIdx] = static_cast<uint8_t>(std::clamp(remapped + 0.5f, 0.f, 255.f));
        }
    };

    // Mips 1–4: sRGB-correct 2×2 box filter
    for (uint32_t m = 1; m < numMips; ++m)
    {
        const uint32_t wSrc = w0 >> (m - 1);
        const uint32_t hSrc = h0 >> (m - 1);
        const uint32_t wDst = w0 >> m;
        const uint32_t hDst = h0 >> m;
        mipData[m].resize(static_cast<size_t>(wDst) * hDst * 4);

        const uint8_t* src = mipData[m - 1].data();
        uint8_t* dst = mipData[m].data();

        for (uint32_t y = 0; y < hDst; ++y)
        {
            for (uint32_t x = 0; x < wDst; ++x)
            {
                const uint32_t sx = x * 2;
                const uint32_t sy = y * 2;
                const uint8_t* p00 = src + (sy * wSrc + sx) * 4;
                const uint8_t* p10 = src + (sy * wSrc + sx + 1) * 4;
                const uint8_t* p01 = src + ((sy + 1) * wSrc + sx) * 4;
                const uint8_t* p11 = src + ((sy + 1) * wSrc + sx + 1) * 4;

                uint8_t* out = dst + (y * wDst + x) * 4;
                if (hasTransparency)
                {
                    const float a00 = p00[3] / 255.f;
                    const float a10 = p10[3] / 255.f;
                    const float a01 = p01[3] / 255.f;
                    const float a11 = p11[3] / 255.f;
                    const float avgA = (a00 + a10 + a01 + a11) * 0.25f;

                    for (int ch = 0; ch < 3; ++ch)
                    {
                        const float avgPremultiplied =
                            (linearize(p00[ch]) * a00 + linearize(p10[ch]) * a10
                             + linearize(p01[ch]) * a01 + linearize(p11[ch]) * a11) * 0.25f;
                        const float avg = avgA > alphaEpsilon ? (avgPremultiplied / avgA) : 0.f;
                        out[ch] = srgbEncode(avg);
                    }

                    out[3] = static_cast<uint8_t>(std::clamp(avgA * 255.f + 0.5f, 0.f, 255.f));
                }
                else
                {
                    for (int ch = 0; ch < 3; ++ch) // RGB: linearize, average, re-encode
                    {
                        const float avg = (linearize(p00[ch]) + linearize(p10[ch])
                                           + linearize(p01[ch]) + linearize(p11[ch])) * 0.25f;
                        out[ch] = srgbEncode(avg);
                    }
                    // Alpha: linear average
                    out[3] = static_cast<uint8_t>((p00[3] + p10[3] + p01[3] + p11[3] + 2) / 4);
                }
            }
        }

        if (hasTransparency)
        {
            preserveAlphaCoverage(mipData[m]);
        }
    }

    return scene->addTexture(std::move(mipData), w0, h0);
}

static std::array<uint32_t, static_cast<size_t>(TerrainMaterial::COUNT)> materialIdxs;

#define MATERIAL_IDX(material) materialIdxs[static_cast<size_t>(material)]

static void createMaterials(Scene* scene)
{
    ToFreeList toFreeList{};

    const uint32_t diffuseTextureId = loadTexture(scene, "diffuse.png");
    if (diffuseTextureId == TEXTURE_ID_INVALID)
    {
        return;
    }

    const uint32_t emissionTextureId = loadTexture(scene, "emission.png");
    if (emissionTextureId == TEXTURE_ID_INVALID)
    {
        return;
    }

    {
        Material defaultMaterial{};
        defaultMaterial.emissiveStrength = 3.0f;
        defaultMaterial.baseColorTextureId = diffuseTextureId;
        defaultMaterial.emissiveColorTextureId = emissionTextureId;
        defaultMaterial.setHasDiffuse(true);
        MATERIAL_IDX(TerrainMaterial::DEFAULT) = scene->addMaterial(toFreeList, &defaultMaterial);
    }

    {
        Material waterMaterial{};
        waterMaterial.baseColor = { 1.f, 1.f, 1.f }; // blue color comes from volume absorption
        waterMaterial.setHasDiffuse(false);
        waterMaterial.setHasGlossyReflection(true);
        waterMaterial.setHasGlossyTransmission(true);
        waterMaterial.ior = 1.33f;
        MATERIAL_IDX(TerrainMaterial::WATER) = scene->addMaterial(toFreeList, &waterMaterial);
    }

    toFreeList.freeAll();
}

uint32_t getMaterialIdx(TerrainMaterial terrainMaterial)
{
    return MATERIAL_IDX(terrainMaterial);
}

} // namespace TerrainMaterials
