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
#include "rendering/renderer.h"
#include "rendering/buffer/acs_helper.h"
#include "rendering/buffer/committed_managed_buffer.h"
#include "rendering/buffer/reserved_managed_buffer.h"
#include "rendering/buffer/mapped_array.h"
#include "rendering/common/common_registers.h"
#include "rendering/common/common_structs.h"

#include <array>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

class ToFreeList;

class Scene;

class Instance
{
    friend class ::Scene;
    friend class ToFreeList;

private:
    ::Scene* const scene;
    const uint32_t id;
    uint32_t materialIdx{ MATERIAL_IDX_INVALID };

    AcsHelper::GeometryWrapper geoWrapper{};
    ManagedBufferSection perTriDatasBufferSection{};

    std::vector<AreaLight> host_areaLights;
    ManagedBufferSection areaLightsBufferSection{};

    bool isVisible{ true };
    bool isScheduledForDeletion{ false };

    Instance(::Scene* scene, uint32_t id);

    void stealVectors(Instance* other);

    void reset(bool alsoFreeFromScene = true);

    DirectX::XMFLOAT3X4 transform{
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
    };
    glm::ivec3 transformOffset{ 0, 0, 0 };

    bool isGeometryFinalized{ false };

public:
    std::vector<Vertex> host_verts{};
    std::vector<uint32_t> host_idxs{};
    std::vector<PerTriangleData> host_perTriDatas{};

    void setTransform(const DirectX::XMFLOAT3X4& transform);
    void setTransformOffset(glm::ivec3 offset);
    void finalizeGeometry();

    // finalizeGeometry() must be called before calling this function
    void addAreaLights(const std::vector<uint32_t>& triangleIdxs);

    uint32_t getId() const;

    uint32_t getTriCount() const;

    bool getIsGeometryFinalized() const;

    void setVisible(bool visible);

    void setMaterialIdx(uint32_t id);
};

class Scene
{
    friend class Instance;
    friend class ToFreeList;

private:
    ReservedManagedBuffer managedVertsBuffer{
        4ull * 1024 * 1024 * 1024, // 4 GB
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        {
            .isResizable = true,
        },
    };
    ReservedManagedBuffer managedIdxsBuffer{
        1ull * 1024 * 1024 * 1024, // 1 GB
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        {
            .isResizable = true,
        },
    };
    ReservedManagedBuffer managedPerTriDatasBuffer{
        1ull * 1024 * 1024 * 1024, // 1 GB
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        {
            .isResizable = true,
        },
    };

    uint32_t maxNumInstances{ 0 };
    // one instance desc array per frame to avoid CPU/GPU race conditions
    std::array<MappedArray<D3D12_RAYTRACING_INSTANCE_DESC>, Renderer::NUM_FRAMES_IN_FLIGHT> mappedInstanceDescsArrays{};
    MappedArray<InstanceData> mappedInstanceDatasArray{};

    std::queue<uint32_t> availableInstanceIds{};
    std::unordered_map<uint32_t, std::unique_ptr<Instance>> instances{};
    std::unordered_set<Instance*> instancesReadyForBlasBuild{};

    std::queue<std::unique_ptr<Instance>> instancesToReuse{};

    // not sure if combining multiple structs into one buffer will lead to alignment problems, but it works for now
    CommittedManagedBuffer sharedBlasUploadBuffer{
        &UPLOAD_HEAP,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        {
            .isResizable = true,
            .isMapped = true,
        },
    };

    ManagedBufferSection tlasBufferSection;
    bool isTlasDirty{ false };
    uint32_t numVisibleBlasesWaitingForTlas{ 0 };

    glm::ivec3 globalInstanceOffset{};
    glm::ivec3 prevGlobalInstanceOffset{};

    uint32_t nextMaterialIdx{ 0 };
    MappedArray<::Material> mappedMaterialsArray;

    std::vector<ComPtr<ID3D12Resource>> textures{};
    struct PendingTexture
    {
        std::vector<uint8_t> data;
        uint32_t width;
        uint32_t height;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
    };
    std::vector<PendingTexture> pendingTextures;

    ReservedManagedBuffer managedAreaLightsBuffer{
        512ull * 1024 * 1024, // 512 MB
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        {
            .isResizable = true,
        },
    };
    uint32_t numAreaLights{ 0 };
    MappedArray<uint32_t> areaLightSamplingStructure;

    void freeInstance(Instance* instance);

    // returns true if TLAS is now dirty
    bool makeQueuedBlases(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

    void makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

    void uploadPendingTextures(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

public:
    void init();

    void reset();

    bool update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

    Instance* requestNewInstance(ToFreeList& toFreeList);
    void markInstanceReadyForBlasBuild(Instance* instance);

    uint32_t addMaterial(ToFreeList& toFreeList, const ::Material* material);

    uint32_t addTexture(std::vector<uint8_t>&& data, uint32_t width, uint32_t height);

    const glm::ivec3& getGlobalInstanceOffset() const;
    const glm::ivec3& getPrevGlobalInstanceOffset() const;

    D3D12_GPU_VIRTUAL_ADDRESS getDevInstanceDatasAddress() const;

    D3D12_GPU_VIRTUAL_ADDRESS getDevMaterialsAddress() const;

    bool hasTlas() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevTlasAddress() const;

    D3D12_GPU_VIRTUAL_ADDRESS getDevVertsBufferAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevIdxsBufferAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevPerTriDatasBufferAddress() const;

    uint32_t getNumAreaLights() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevAreaLightsBufferAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevAreaLightSamplingStructureAddress() const;
};
