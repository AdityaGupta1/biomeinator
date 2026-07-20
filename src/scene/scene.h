// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

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
    // If true, geometry gets displaced by a compute pass every frame and its BLAS refit instead of rebuilt
    bool isDeformable{ false };

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

    void setIsDeformable(bool deformable);
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
            .bufferCreationFlags = {
                .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, // deformable instance displacement writes verts in place
            },
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
    // finalized, BLAS-built deformable instances; drives the displacement dispatches and
    // BLAS refits (every deformable instance is water for now)
    std::unordered_set<Instance*> deformableInstances{};

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

    glm::ivec3 globalInstanceOffset{};
    glm::ivec3 prevGlobalInstanceOffset{};

    bool hasSceneBounds{ false };
    glm::vec3 sceneBoundsMin_WS{};
    glm::vec3 sceneBoundsMax_WS{};

    uint32_t nextMaterialIdx{ 0 };
    MappedArray<::Material> mappedMaterialsArray;

    std::vector<ComPtr<ID3D12Resource>> textures{};
    struct PendingTexture
    {
        // sliceMipData[slice][mip]; size = arraySize.
        std::vector<std::vector<std::vector<uint8_t>>> sliceMipData;
        uint32_t width;  // mip 0, per slice
        uint32_t height; // mip 0, per slice
        uint32_t arraySize;
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
    // High water mark of the sparse area-light index (== max sparse index in
    // areaLightSamplingStructure + 1, this frame). Used by callers that index
    // parallel buffers keyed by the sparse areaLights[] index.
    uint32_t areaLightSparseCount{ 0 };
    // Set when makeTlas rewrites areaLightSamplingStructure; cleared at the
    // start of the next Scene::update.
    bool areaLightTopologyChanged{ false };
    MappedArray<uint32_t> areaLightSamplingStructure;

    void freeInstance(Instance* instance);

    // returns true if TLAS is now dirty
    bool makeQueuedBlases(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

    void updateDeformableInstances(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, float animTime);

    void makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, bool updateAreaLights);

    void uploadPendingTextures(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

public:
    void init();

    void reset();

    bool update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, float animTime);

    Instance* requestNewInstance(ToFreeList& toFreeList);
    void markInstanceReadyForBlasBuild(Instance* instance);

    uint32_t addMaterial(ToFreeList& toFreeList, const ::Material* material);

    uint32_t addTexture(std::vector<std::vector<uint8_t>>&& mipData, uint32_t width, uint32_t height);
    uint32_t addTexture(std::vector<uint8_t>&& mip0, uint32_t width, uint32_t height);
    uint32_t addTextureArray(
        std::vector<std::vector<std::vector<uint8_t>>>&& sliceMipData, uint32_t width, uint32_t height);

    void expandBounds(const glm::vec3& pos_WS);
    bool hasBounds() const;
    const glm::vec3& getBoundsMin_WS() const;
    const glm::vec3& getBoundsMax_WS() const;

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
    uint32_t getAreaLightSparseCount() const;
    bool didAreaLightTopologyChange() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevAreaLightsBufferAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevAreaLightSamplingStructureAddress() const;
};
