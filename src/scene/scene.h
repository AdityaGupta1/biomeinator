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

// An instance's CPU-side geometry vectors. They are only needed until the instance's BLAS inputs
// are uploaded, so they are then emptied and pooled for a later instance.
struct HostGeometry
{
    std::vector<Vertex> verts;
    // Optional resident form of verts (same count) in the layout Instance::packedVertexFormat names;
    // verts then only feeds the BLAS build and area lights, see knowledge/scene/instance.md
    std::vector<PackedTerrainVertex> packedTerrainVerts;
    std::vector<VertexTangent> tangents; // optional, indexed like verts
    std::vector<uint32_t> idxs; // empty for quad faces, see Instance::hasQuadFaces()
    // One entry per 1 << Instance::trisPerFaceLog2 triangles
    std::vector<PerFaceData> perFaceDatas;
    // Per-triangle OMM Array indices (or special indices); empty for non-OMM geometry
    std::vector<uint16_t> ommIdxs;
    std::vector<AreaLight> areaLights;

    size_t capacityBytes() const;
    // Empties every vector, keeping its capacity
    void clear();
};

// Pooled sets keep their capacity, so instances with small meshes (e.g. water) use their own pool
// rather than tying up sets grown by large ones
enum class HostGeometrySize : uint8_t
{
    LARGE,
    SMALL,
    COUNT,
};

class Instance
{
    friend class ::Scene;
    friend class ToFreeList;

private:
    ::Scene* const scene;
    const uint32_t id;
    uint32_t materialIdx{ MATERIAL_IDX_INVALID };

    AcsHelper::GeometryWrapper geoWrapper{};
    ManagedBufferSection perFaceDatasBufferSection{};
    ManagedBufferSection tangentsBufferSection{};

    ManagedBufferSection areaLightsBufferSection{};

    bool isVisible{ true };
    bool isScheduledForDeletion{ false };
    // If true, geometry gets displaced by a compute pass every frame and its BLAS refit instead of rebuilt
    bool isDeformable{ false };
    // If true, the BLAS geometry is flagged opaque so traversal never invokes anyhit for it
    bool isOpaque{ false };
    // See HostGeometry::perFaceDatas
    uint32_t trisPerFaceLog2{ 0 };
    // VERTEX_FORMAT_* of HostGeometry::packedTerrainVerts, when there are any
    uint32_t packedVertexFormat{ VERTEX_FORMAT_PACKED_TERRAIN };
    // See setHeightfieldCornersPerRow()
    uint32_t heightfieldCornersPerRow{ 0 };

    // Faces are quads whose indices are implicit (see getQuadFaceVertIdx()) rather than stored
    bool hasQuadFaces() const;
    bool hasHeightfieldFaces() const;

    Instance(::Scene* scene, uint32_t id);

    void takeHostGeometry(HostGeometry&& geometry);
    void releaseHostGeometry();

    void reset(bool alsoFreeFromScene = true);

    DirectX::XMFLOAT3X4 transform{
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
    };
    glm::ivec3 transformOffset{ 0, 0, 0 };

    bool isGeometryFinalized{ false };
    // From creation until its geometry is uploaded or it is destroyed
    bool holdsHostGeometry{ false };
    HostGeometrySize hostGeometrySize{ HostGeometrySize::LARGE };
    glm::vec3 boundsMin_OS{ 0.f, 0.f, 0.f }; // of hostGeometry.verts, set by finalizeGeometry
    glm::vec3 boundsMax_OS{ 0.f, 0.f, 0.f };
    uint32_t tlasEntryIdx{ UINT32_MAX }; // index into Scene::tlasInstanceEntries while in the TLAS

public:
    HostGeometry hostGeometry{};

    void setTransform(const DirectX::XMFLOAT3X4& transform);
    void setTransformOffset(glm::ivec3 offset);
    void finalizeGeometry();

    // finalizeGeometry() must be called before calling this function
    void addAreaLights(const std::vector<uint32_t>& triangleIdxs);

    uint32_t getId() const;

    uint32_t getTriCount() const;

    bool getIsGeometryFinalized() const;
    // Whether its BLAS is built, so showing it puts it in the TLAS this frame
    bool getHasBlas() const;

    void setVisible(bool visible);

    void setMaterialIdx(uint32_t id);

    void setIsDeformable(bool deformable);

    void setIsOpaque(bool opaque);

    // Must be set before finalizeGeometry(); the triangle count must be a multiple of the face size
    void setTrisPerFaceLog2(uint32_t log2);

    void setPackedVertexFormat(uint32_t vertexFormat);

    // Nonzero marks a LOD heightfield, whose faces each record where their verts are (see
    // getHeightfieldFaceVertIdx()), so its indices only feed the BLAS build and are never kept resident.
    // Must be set before finalizeGeometry().
    void setHeightfieldCornersPerRow(uint32_t cornersPerRow);
};

class Scene
{
    friend class Instance;
    friend class ToFreeList;

private:
    // Both vertex layouts share one buffer whose sections are aligned to the larger stride, so the
    // smaller one must divide it or its element offsets would not be whole
    static_assert(sizeof(Vertex) % sizeof(PackedTerrainVertex) == 0);
    // Shaders index the typed scene buffers with 32-bit element indices, so a buffer read as a
    // StructuredBuffer cannot usefully exceed 4 GB: sections past that mark trace fine (the BLAS
    // takes a 64-bit VA) but shade from wrapped-around garbage
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
        4ull * 1024 * 1024 * 1024, // 4 GB
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        {
            .isResizable = true,
            .alignmentBytes = sizeof(uint32_t),
        },
    };
    ReservedManagedBuffer managedPerFaceDatasBuffer{
        1ull * 1024 * 1024 * 1024, // 1 GB
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        {
            .isResizable = true,
            .alignmentBytes = sizeof(PerFaceData),
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
    // One batch of compaction queries per frame context: the batch recorded on frame index i is
    // compacted when index i comes around again, once its fence has passed; see
    // knowledge/gpu/acceleration_structures.md
    struct PendingBlasCompaction
    {
        AcsHelper::BlasCompactionQuery query;
        // The builds behind the query's entries, in order. Instances can be destroyed before
        // the slot is consumed, so they are resolved by id when it is
        struct Build
        {
            uint32_t instanceId;
            uint32_t blasBuildId;
        };
        std::vector<Build> builds;
    };
    std::array<PendingBlasCompaction, Renderer::NUM_FRAMES_IN_FLIGHT> pendingBlasCompactions{};
    void compactBuiltBlases(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);
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
    float waveFrustumHeightBand{ FLT_MAX };

    // Never freed while running; bounded because callers cap how many instances hold geometry.
    // See knowledge/scene/instance.md
    std::array<std::vector<HostGeometry>, static_cast<size_t>(HostGeometrySize::COUNT)> hostGeometryPools{};
    std::array<uint32_t, static_cast<size_t>(HostGeometrySize::COUNT)> numInstancesHoldingHostGeometry{};

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
        uint32_t areaLightDenseOffset; // where its block sits in the sampling structure
    };
    std::vector<TlasInstanceEntry> tlasInstanceEntries;
    bool tlasEntriesNeedCompaction{ false };
    // Instances whose visibility flipped on outside of update(), added once a ToFreeList is at hand
    std::vector<Instance*> pendingTlasEntryAdds;
    void addTlasEntry(Instance* instance, ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);
    void removeTlasEntry(Instance* instance);
    void compactTlasEntries(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);
    // Appends [sparseOffset, sparseOffset + count) to the sampling structure on the device
    void appendAreaLightSamplingRange(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, uint32_t sparseOffset, uint32_t count);
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
    void recycleHostGeometry(HostGeometrySize size, HostGeometry&& geometry);

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

    struct InstanceMemory
    {
        uint32_t numInstances{ 0 };
        size_t blasBytes{ 0 };
        size_t vertsBytes{ 0 };
        size_t idxsBytes{ 0 };
        size_t ommIdxsBytes{ 0 };
        size_t perFaceDatasBytes{ 0 };
        size_t tangentsBytes{ 0 };
        size_t areaLightsBytes{ 0 };
        // CPU-side copies (the host_ vectors), by capacity
        size_t hostBytes{ 0 };
    };
    // Sums the buffer sections held by every instance (in the TLAS or not) of one kind
    InstanceMemory getInstanceMemory(bool deformable) const;
    size_t getHostGeometryPoolBytes() const;
    // Instances between creation and upload; a pool never holds more sets than the peak of this
    uint32_t getNumInstancesHoldingHostGeometry(HostGeometrySize size) const;

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

    Instance* requestNewInstance(ToFreeList& toFreeList, HostGeometrySize hostGeometrySize = HostGeometrySize::LARGE);
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
    D3D12_GPU_VIRTUAL_ADDRESS getDevPerFaceDatasBufferAddress() const;

    uint32_t getNumAreaLights() const;
    uint32_t getAreaLightSparseCount() const;
    bool didAreaLightTopologyChange() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevAreaLightsBufferAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevAreaLightSamplingStructureAddress() const;
};
