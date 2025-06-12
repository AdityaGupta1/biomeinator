#pragma once

#include "dxr_includes.h"

#include "common_structs.h"
#include "host_structs.h"
#include "buffer/acs_helper.h"

#include <unordered_map>
#include <memory>
#include <queue>

class ToFreeList;

class Scene;

class Instance
{
    friend class ToFreeList;

    friend class Scene;

private:
    Scene* const scene;
    const uint32_t id;

    AcsHelper::GeometryWrapper geoWrapper;
    bool hasBlas{ false };

    Instance(Scene* scene, uint32_t id);

public:
    std::vector<Vertex> host_verts;
    std::vector<uint32_t> host_idxs;

    DirectX::XMFLOAT3X4 transform{};

    void markReadyForBlasBuild();
};

class Scene
{
    friend class Instance;

private:
    const uint32_t maxNumInstances;

    ManagedBuffer dev_vertBuffer{
        &DEFAULT_HEAP,
        D3D12_HEAP_FLAG_NONE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        false /*isMapped*/,
    };
    ManagedBuffer dev_idxBuffer{
        &DEFAULT_HEAP,
        D3D12_HEAP_FLAG_NONE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
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

    bool makeQueuedBlasesAndUpdateInstances(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);
    void makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

public:
    Scene(uint32_t maxNumInstances);

    void init(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

    void update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

    Instance* requestNewInstance();

    ID3D12Resource* getDevInstanceDescs();
    ID3D12Resource* getDevInstanceDatas();

    ID3D12Resource* getDevTlas();

    ID3D12Resource* getDevVertBuffer();
    ID3D12Resource* getDevIdxBuffer();
};
