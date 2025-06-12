#pragma once

#include "dxr_includes.h"

#include "buffer/acs_helper.h"
#include "common_structs.h"
#include "host_structs.h"

class ToFreeList;

class Scene
{
    friend class ToFreeList;

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

    AcsHelper::GeometryWrapper quadGeoWrapper{};
    AcsHelper::GeometryWrapper cubeGeoWrapper{};

    ComPtr<ID3D12Resource> dev_tlas{ nullptr };

    void makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

public:
    Scene(uint32_t maxNumInstances);

    void init(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

    ID3D12Resource* getDevInstanceDescs();
    ID3D12Resource* getDevInstanceDatas();

    ID3D12Resource* getDevTlas();

    ID3D12Resource* getDevVertBuffer();
    ID3D12Resource* getDevIdxBuffer();
};
