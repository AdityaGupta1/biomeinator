// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "biome_map.h"

#include "camera.h"
#include "dxr_common.h"
#include "renderer.h"
#include "rendering/buffer/buffer_helper.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/common/common_settings.h"
#include "settings_manager.h"
#include "terrain/biome.h"
#include "terrain/chunk.h"
#include "terrain/chunk_generator.h"
#include "util/math.h"

#include <vector>

namespace BiomeMap
{

namespace
{

ComPtr<ID3D12Resource> dev_texture{ nullptr };
uint32_t srvIdx{ 0 };

uint32_t texelsPerSide{ 0 };
glm::ivec2 originBlocksXZ_WS{ 0, 0 };
uint32_t filledWorldSeed{ 0 };

} // namespace

void update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    const uint32_t renderDistance = static_cast<uint32_t>(SettingsManager::getAsInt("renderDistance"));
    const uint32_t newTexelsPerSide = (2u * renderDistance + 1u) * chunkSizeXZ / BIOME_MAP_BLOCKS_PER_TEXEL;

    const glm::ivec3 cameraPosInt_WS = Renderer::getCamera().getPosInt_WS();
    constexpr int blocksPerTexel = BIOME_MAP_BLOCKS_PER_TEXEL;
    const int halfExtentBlocks = static_cast<int>(newTexelsPerSide / 2u) * blocksPerTexel;
    const glm::ivec2 newOriginBlocksXZ_WS = {
        MathUtil::floorDiv(cameraPosInt_WS.x, blocksPerTexel) * blocksPerTexel - halfExtentBlocks,
        MathUtil::floorDiv(cameraPosInt_WS.z, blocksPerTexel) * blocksPerTexel - halfExtentBlocks,
    };

    const uint32_t worldSeed = SettingsManager::getWorldSeed();

    const bool needsRecreate = (dev_texture == nullptr || newTexelsPerSide != texelsPerSide);
    const bool needsRefill = needsRecreate || newOriginBlocksXZ_WS != originBlocksXZ_WS || worldSeed != filledWorldSeed;
    if (!needsRefill)
    {
        return;
    }

    texelsPerSide = newTexelsPerSide;
    originBlocksXZ_WS = newOriginBlocksXZ_WS;
    filledWorldSeed = worldSeed;

    if (needsRecreate)
    {
        if (dev_texture != nullptr)
        {
            toFreeList.pushResource(dev_texture, false);
            toFreeList.pushDescriptor(srvIdx);
        }

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = texelsPerSide;
        texDesc.Height = texelsPerSide;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        texDesc.SampleDesc = SAMPLE_DESC_NO_AA;

        CHECK_HRESULT(Renderer::getDevice()->CreateCommittedResource(&DEFAULT_HEAP,
                                                                     D3D12_HEAP_FLAG_NONE,
                                                                     &texDesc,
                                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                                     nullptr,
                                                                     IID_PPV_ARGS(&dev_texture)));
        dev_texture->SetName(L"biomeMap");

        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle;
        srvIdx = Renderer::sharedDescHeapAlloc.alloc(&srvCpuHandle);
        const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
            .Format = texDesc.Format,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = { .MipLevels = 1 },
        };
        Renderer::getDevice()->CreateShaderResourceView(dev_texture.Get(), &srvDesc, srvCpuHandle);
    }

    std::vector<Biome> biomes(static_cast<size_t>(texelsPerSide) * texelsPerSide);
    ChunkGenerator::fillBiomeRect(
        biomes.data(), originBlocksXZ_WS, glm::uvec2(texelsPerSide, texelsPerSide), blocksPerTexel);

    const uint32_t rowPitchBytes = texelsPerSide * 4;
    const uint32_t rowPitchBytesAligned = MathUtil::roundUp(rowPitchBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

    ComPtr<ID3D12Resource> dev_uploadBuffer =
        BufferHelper::createBasicBuffer(static_cast<uint64_t>(rowPitchBytesAligned) * texelsPerSide, &UPLOAD_HEAP);
    dev_uploadBuffer->SetName(L"biomeMap upload");
    uint8_t* host_uploadBuffer = nullptr;
    dev_uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&host_uploadBuffer));

    for (uint32_t texelZ = 0; texelZ < texelsPerSide; ++texelZ)
    {
        uint8_t* destRow = host_uploadBuffer + static_cast<size_t>(texelZ) * rowPitchBytesAligned;
        for (uint32_t texelX = 0; texelX < texelsPerSide; ++texelX)
        {
            const Biome biome = biomes[texelX + texelsPerSide * texelZ];
            const glm::vec3& tint = Biomes::getBiomeData(biome).grassTint;
            uint8_t* destTexel = destRow + texelX * 4;
            destTexel[0] = static_cast<uint8_t>(tint.r * 255.f + 0.5f);
            destTexel[1] = static_cast<uint8_t>(tint.g * 255.f + 0.5f);
            destTexel[2] = static_cast<uint8_t>(tint.b * 255.f + 0.5f);
            destTexel[3] = 255;
        }
    }

    dev_uploadBuffer->Unmap(0, nullptr);

    BufferHelper::stateTransitionResourceBarrier(
        cmdList, dev_texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    const D3D12_TEXTURE_COPY_LOCATION srcTexLocation = {
        .pResource = dev_uploadBuffer.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = {
            .Offset = 0,
            .Footprint = {
                .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                .Width = texelsPerSide,
                .Height = texelsPerSide,
                .Depth = 1,
                .RowPitch = rowPitchBytesAligned,
            },
        },
    };
    const D3D12_TEXTURE_COPY_LOCATION destTexLocation = {
        .pResource = dev_texture.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    cmdList->CopyTextureRegion(&destTexLocation, 0, 0, 0, &srcTexLocation, nullptr);

    BufferHelper::stateTransitionResourceBarrier(
        cmdList, dev_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    toFreeList.pushResource(dev_uploadBuffer, false);
}

uint32_t getSrvIdx()
{
    return srvIdx;
}

glm::ivec2 getOriginBlocksXZ_WS()
{
    return originBlocksXZ_WS;
}

uint32_t getTexelsPerSide()
{
    return texelsPerSide;
}

void destroy()
{
    if (dev_texture != nullptr)
    {
        Renderer::sharedDescHeapAlloc.free(srvIdx);
        dev_texture.Reset();
    }
    texelsPerSide = 0;
}

} // namespace BiomeMap
