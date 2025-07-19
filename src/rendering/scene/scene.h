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

class Instance
{
    friend class Scene;
    friend class ToFreeList;

private:
    Scene* const scene;
    const uint32_t id;
    uint32_t materialId{ MATERIAL_ID_INVALID };

    AcsHelper::GeometryWrapper geoWrapper{};
    ManagedBufferSection areaLightsBufferSection{};

    bool isScheduledForDeletion{ false };

    Instance(Scene* scene, uint32_t id);

public:
    std::vector<Vertex> host_verts;
    std::vector<uint32_t> host_idxs;

    DirectX::XMFLOAT3X4 transform{};

    std::vector<AreaLight> host_areaLights;

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
