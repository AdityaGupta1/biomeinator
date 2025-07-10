#pragma once

#include "rendering/common_structs.h"
#include "rendering/dxr_includes.h"
#include "rendering/host_structs.h"
#include "rendering/buffer/acs_helper.h"

#include <array>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>

constexpr uint32_t MAX_NUM_TEXTURES = 16;

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

    AcsHelper::GeometryWrapper geoWrapper;

    bool isScheduledForDeletion{ false };

    Instance(Scene* scene, uint32_t id);

public:
    std::vector<Vertex> host_verts;
    std::vector<uint32_t> host_idxs;

    DirectX::XMFLOAT3X4 transform{};

    void setMaterialId(uint32_t id);
};

class Scene
{
    friend class Instance;
    friend class ToFreeList;

private:
    uint32_t maxNumInstances{ 0 };

    ManagedBuffer dev_vertBuffer{
        &DEFAULT_HEAP,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        true /*isResizable*/,
        false /*isMapped*/,
    };
    ManagedBuffer dev_idxBuffer{
        &DEFAULT_HEAP,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        true /*isResizable*/,
        false /*isMapped*/,
    };

    D3D12_RAYTRACING_INSTANCE_DESC* host_instanceDescs{ nullptr };
    ComPtr<ID3D12Resource> dev_instanceDescs{ nullptr };
    InstanceData* host_instanceDatas{ nullptr };
    ComPtr<ID3D12Resource> dev_instanceDatas{ nullptr };

    std::queue<uint32_t> availableInstanceIds;
    std::unordered_map<uint32_t, std::unique_ptr<Instance>> instances;
    std::vector<Instance*> instancesReadyForBlasBuild;

    ComPtr<ID3D12Resource> dev_tlas{ nullptr };
    bool isTlasDirty{ false };

    uint32_t maxNumMaterials{ 0 };
    uint32_t nextMaterialId{ 0 };

    Material* host_materials{ nullptr };
    ComPtr<ID3D12Resource> dev_materials{ nullptr };

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

    void initInstanceBuffers();
    void resizeInstanceBuffers(ToFreeList& toFreeList, uint32_t newNumInstances);
    void freeInstance(Instance* instance);

    void initMaterialBuffers();
    void resizeMaterialBuffers(ToFreeList& toFreeList, uint32_t newNumMaterials);

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

    uint32_t addTexture(std::vector<uint8_t> data, uint32_t width, uint32_t height);

    ID3D12Resource* getDevInstanceDescs();
    ID3D12Resource* getDevInstanceDatas();

    ID3D12Resource* getDevMaterials();

    ID3D12Resource* getDevTlas();

    ID3D12Resource* getDevVertBuffer();
    ID3D12Resource* getDevIdxBuffer();

};
