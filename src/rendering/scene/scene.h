/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

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

#include "rendering/dxr_includes.h"
#include "rendering/host_structs.h"
#include "rendering/buffer/acs_helper.h"
#include "rendering/buffer/mapped_array.h"
#include "rendering/common/common_registers.h"
#include "rendering/common/common_structs.h"

#include <array>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>

class ToFreeList;

class Scene;

struct AreaLightInputs
{
    DirectX::XMFLOAT3 pos0;
    DirectX::XMFLOAT3 pos1;
    DirectX::XMFLOAT3 pos2;
    uint32_t triangleIdx;
};

class Instance
{
    friend class Scene;
    friend class ToFreeList;

private:
    Scene* const scene;
    const uint32_t id;
    uint32_t materialId{ MATERIAL_ID_INVALID };

    AcsHelper::GeometryWrapper geoWrapper{};

    std::vector<AreaLight> host_areaLights;
    ManagedBufferSection areaLightsBufferSection{};

    bool isScheduledForDeletion{ false };

    Instance(Scene* scene, uint32_t id);

public:
    std::vector<Vertex> host_verts;
    std::vector<uint32_t> host_idxs;

    DirectX::XMFLOAT3X4 transform{};

    // `transform` must be set before calling this function
    void addAreaLight(const AreaLightInputs& lightInputs);

    uint32_t getId() const;

    void setMaterialId(uint32_t id);
};

class Scene
{
    friend class Instance;
    friend class ToFreeList;

private:
    ManagedBuffer managedVertsBuffer{
        &DEFAULT_HEAP,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        true /*isResizable*/,
        false /*isMapped*/,
    };
    ManagedBuffer managedIdxsBuffer{
        &DEFAULT_HEAP,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        true /*isResizable*/,
        false /*isMapped*/,
    };

    uint32_t maxNumInstances{ 0 };
    MappedArray<D3D12_RAYTRACING_INSTANCE_DESC> mappedInstanceDescsArray{};
    MappedArray<InstanceData> mappedInstanceDatasArray{};

    std::queue<uint32_t> availableInstanceIds{};
    std::unordered_map<uint32_t, std::unique_ptr<Instance>> instances{};
    std::vector<Instance*> instancesReadyForBlasBuild{};

    ComPtr<ID3D12Resource> dev_tlas{ nullptr };
    bool isTlasDirty{ false };

    uint32_t nextMaterialIdx{ 0 };
    MappedArray<Material> mappedMaterialsArray{};

    uint32_t nextTextureId{ 0 };
    std::array<ComPtr<ID3D12Resource>, MAX_NUM_TEXTURES> textures{};
    struct PendingTexture
    {
        std::vector<uint8_t> data;
        uint32_t width;
        uint32_t height;
        uint32_t id;
    };
    std::vector<PendingTexture> pendingTextures;

    ManagedBuffer managedAreaLightsBuffer{
        &DEFAULT_HEAP,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        true /*isResizable*/,
        false /*isMapped*/,
    };
    uint32_t numAreaLights{ 0 };
    MappedArray<uint32_t> areaLightSamplingStructure{};

    void freeInstance(Instance* instance);

    bool makeQueuedBlases(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);
    void makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

    void uploadPendingTextures(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

public:
    void init();

    void clear();

    void update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

    Instance* requestNewInstance(ToFreeList& toFreeList);
    void markInstanceReadyForBlasBuild(Instance* instance);

    uint32_t addMaterial(ToFreeList& toFreeList, const Material* material);

    uint32_t addTexture(std::vector<uint8_t>&& data, uint32_t width, uint32_t height);

    D3D12_GPU_VIRTUAL_ADDRESS getDevInstanceDatasAddress() const;

    D3D12_GPU_VIRTUAL_ADDRESS getDevMaterialsAddress() const;

    bool hasTlas() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevTlasAddress() const;

    D3D12_GPU_VIRTUAL_ADDRESS getDevVertsBufferAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevIdxsBufferAddress() const;

    uint32_t getNumAreaLights() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevAreaLightsBufferAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevAreaLightSamplingStructureAddress() const;
};
