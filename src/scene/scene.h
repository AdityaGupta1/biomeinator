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
#include "rendering/common/common_params.h"
#include "rendering/common/common_structs.h"

#include <array>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include <array>
#include <cfloat>

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
    ManagedBufferSection tangentsBufferSection{};

    std::vector<AreaLight> host_areaLights;
    ManagedBufferSection areaLightsBufferSection{};

    bool isVisible{ true };
    bool isScheduledForDeletion{ false };
    // If true, geometry gets displaced by a compute pass every frame and its BLAS refit instead of rebuilt
    bool isDeformable{ false };
    // If true, the BLAS geometry is flagged opaque so traversal never invokes anyhit for it
    bool isOpaque{ false };

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
    glm::vec3 boundsMin_OS{ 0.f, 0.f, 0.f }; // of host_verts, set by finalizeGeometry
    glm::vec3 boundsMax_OS{ 0.f, 0.f, 0.f };
    uint32_t tlasEntryIdx{ UINT32_MAX }; // index into Scene::tlasInstanceEntries while in the TLAS

public:
    std::vector<Vertex> host_verts{};
    std::vector<VertexTangent> host_tangents{}; // optional, indexed like host_verts
    std::vector<uint32_t> host_idxs{};
    std::vector<PerTriangleData> host_perTriDatas{};
    // Per-triangle OMM Array indices (or special indices); empty for non-OMM geometry
    std::vector<uint16_t> host_ommIdxs{};

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

    void setIsOpaque(bool opaque);
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
            .alignmentBytes = sizeof(Vertex),
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
            .alignmentBytes = sizeof(uint32_t),
        },
    };
    ReservedManagedBuffer managedPerTriDatasBuffer{
        1ull * 1024 * 1024 * 1024, // 1 GB
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        {
            .isResizable = true,
            .alignmentBytes = sizeof(PerTriangleData),
        },
    };

    CommittedManagedBuffer managedTangentsBuffer{
        &DEFAULT_HEAP,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        { .isResizable = true, .alignmentBytes = sizeof(VertexTangent) },
    };

    uint32_t maxNumInstances{ 0 };
    // one instance desc array per frame to avoid CPU/GPU race conditions
    std::array<MappedArray<D3D12_RAYTRACING_INSTANCE_DESC>, Renderer::NUM_FRAMES_IN_FLIGHT> mappedInstanceDescsArrays{};
    MappedArray<InstanceData> mappedInstanceDatasArray{};

    std::queue<uint32_t> availableInstanceIds{};
    std::unordered_map<uint32_t, std::unique_ptr<Instance>> instances{};
    std::unordered_set<Instance*> instancesReadyForBlasBuild{};
    uint32_t numBlasBuilds{ 0 }; // lifetime total, for streaming measurements
    // finalized, BLAS-built deformable instances; drives the displacement dispatches and
    // BLAS refits (every deformable instance is water for now)
    std::unordered_set<Instance*> deformableInstances{};
    // The subset inside the animation bounds, rebuilt when the bounds or the set change
    std::vector<Instance*> animatedDeformables{};
    bool animatedDeformablesDirty{ true };
    // Instances within this radius of the center and inside the padded frustum are animated.
    // waveFade is what the shaders use; its defaults (huge radii, zero normals) keep everything
    // animated at full amplitude for scenes that never set them (glTF)
    glm::vec2 deformableAnimCenterXZ_WS{ 0.f, 0.f };
    float deformableAnimRadius{ FLT_MAX };
    WaveFadeParams waveFade{ { 0.f, 0.f, 0.f }, 1e9f, 2e9f, 0.f, 0.f, 0.f, {} };
    bool waveFrustumSet{ false };

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
    // The instances currently in the TLAS (visible, not scheduled for deletion, BLAS built),
    // maintained incrementally so the per-frame TLAS rebuild only re-applies the global offset
    // to a contiguous array instead of walking every instance; see knowledge/scene/scene.md
    struct TlasInstanceEntry
    {
        D3D12_RAYTRACING_INSTANCE_DESC desc; // translation excludes transformOffset
        glm::ivec3 transformOffset;
        Instance* instance; // null once removed, until the next compaction
        uint32_t areaLightSparseOffset;
        uint32_t numAreaLights;
    };
    std::vector<TlasInstanceEntry> tlasInstanceEntries;
    bool tlasEntriesNeedCompaction{ false };
    // Instances whose visibility flipped on outside of update(), added once a ToFreeList is at hand
    std::vector<Instance*> pendingTlasEntryAdds;
    // CPU master copy of areaLightSamplingStructure: the mapped array only stages the ranges
    // written into the current frame's slot, so appends and compactions are staged from here
    std::vector<uint32_t> areaLightDenseIdxs;
    void addTlasEntry(Instance* instance, ToFreeList& toFreeList);
    void removeTlasEntry(Instance* instance);
    void compactTlasEntries();
    void stageAreaLightSamplingRange(ToFreeList& toFreeList, uint32_t start, uint32_t count);
    // Global radiance changes, distinct from streamed instance/TLAS updates.
    bool radianceHistoryInvalidated{ false };

    glm::ivec3 globalInstanceOffset{};
    glm::ivec3 prevGlobalInstanceOffset{};

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
        DXGI_FORMAT format;
    };
    std::vector<PendingTexture> pendingTextures;

    ReservedManagedBuffer managedAreaLightsBuffer{
        512ull * 1024 * 1024, // 512 MB
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        {
            .isResizable = true,
            .alignmentBytes = sizeof(AreaLight),
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
    void makeQueuedBlases(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

    void updateDeformableInstances(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, float waveTime);

    void makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

    void uploadPendingTextures(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

public:
    void init();

    void reset();
    void invalidateRadianceHistory();
    bool consumeRadianceHistoryInvalidation();

    bool update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, float waveTime);

    uint32_t getNumBlasBuilds() const
    {
        return this->numBlasBuilds;
    }

    // Deformable instances outside animRadius of the center or outside the padded frustum are
    // left static; the shaders fade the waves to rest height towards both limits so the two
    // regions meet flat
    void setDeformableAnimation(glm::vec2 centerXZ_WS, float animRadius, float fadeStart, float fadeEnd);
    void setWaveFrustum(glm::vec3 cameraPos_WS, const std::array<glm::vec3, 4>& sideNormals_WS);
    bool isDeformableAnimated(const Instance* instance) const;
    const WaveFadeParams& getWaveFade() const
    {
        return this->waveFade;
    }

    Instance* requestNewInstance(ToFreeList& toFreeList);
    void markInstanceReadyForBlasBuild(Instance* instance);

    uint32_t addMaterial(ToFreeList& toFreeList, const ::Material* material);

    uint32_t addTexture(std::vector<std::vector<uint8_t>>&& mipData, uint32_t width, uint32_t height,
                        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    uint32_t addTexture(std::vector<uint8_t>&& mip0, uint32_t width, uint32_t height,
                        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    uint32_t addTextureArray(std::vector<std::vector<std::vector<uint8_t>>>&& sliceMipData,
                             uint32_t width,
                             uint32_t height,
                             DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);

    const glm::ivec3& getGlobalInstanceOffset() const;
    const glm::ivec3& getPrevGlobalInstanceOffset() const;

    D3D12_GPU_VIRTUAL_ADDRESS getDevInstanceDatasAddress() const;

    D3D12_GPU_VIRTUAL_ADDRESS getDevMaterialsAddress() const;

    bool hasTlas() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevTlasAddress() const;

    D3D12_GPU_VIRTUAL_ADDRESS getDevVertsBufferAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevTangentsBufferAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevIdxsBufferAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevPerTriDatasBufferAddress() const;

    uint32_t getNumAreaLights() const;
    uint32_t getAreaLightSparseCount() const;
    bool didAreaLightTopologyChange() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevAreaLightsBufferAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevAreaLightSamplingStructureAddress() const;
};
