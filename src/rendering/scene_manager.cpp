#include "scene_manager.h"

#include "dxr_common.h"
#include "renderer.h"
#include "buffer/acs_helper.h"
#include "buffer/buffer_helper.h"

namespace SceneManager
{

const std::vector<Vertex> quadVerts = {
    {{-1, 0, -1}, {0, 1, 0}, {0, 0}},
    {{-1, 0, 1}, {0, 1, 0}, {0, 1}},
    {{1, 0, 1}, {0, 1, 0}, {1, 1}},
    {{-1, 0, -1}, {0, 1, 0}, {0, 0}},
    {{1, 0, -1}, {0, 1, 0}, {1, 0}},
    {{1, 0, 1}, {0, 1, 0}, {1, 1}},
};
const std::vector<Vertex> cubeVerts = {
    // -x (left)
    {{-1, -1, -1}, {-1, 0, 0}, {0, 1}},
    {{-1, -1, 1}, {-1, 0, 0}, {1, 1}},
    {{-1, 1, 1}, {-1, 0, 0}, {1, 0}},
    {{-1, 1, -1}, {-1, 0, 0}, {0, 0}},

    // -y (bottom)
    {{-1, -1, -1}, {0, -1, 0}, {0, 0}},
    {{1, -1, -1}, {0, -1, 0}, {1, 0}},
    {{1, -1, 1}, {0, -1, 0}, {1, 1}},
    {{-1, -1, 1}, {0, -1, 0}, {0, 1}},

    // -z (back)
    {{-1, -1, -1}, {0, 0, -1}, {1, 1}},
    {{-1, 1, -1}, {0, 0, -1}, {1, 0}},
    {{1, 1, -1}, {0, 0, -1}, {0, 0}},
    {{1, -1, -1}, {0, 0, -1}, {0, 1}},

    // +x (right)
    {{1, -1, -1}, {1, 0, 0}, {1, 1}},
    {{1, 1, -1}, {1, 0, 0}, {1, 0}},
    {{1, 1, 1}, {1, 0, 0}, {0, 0}},
    {{1, -1, 1}, {1, 0, 0}, {0, 1}},

    // +y (top)
    {{-1, 1, -1}, {0, 1, 0}, {0, 0}},
    {{-1, 1, 1}, {0, 1, 0}, {0, 1}},
    {{1, 1, 1}, {0, 1, 0}, {1, 1}},
    {{1, 1, -1}, {0, 1, 0}, {1, 0}},

    // +z (front)
    {{-1, -1, 1}, {0, 0, 1}, {0, 1}},
    {{1, -1, 1}, {0, 0, 1}, {1, 1}},
    {{1, 1, 1}, {0, 0, 1}, {1, 0}},
    {{-1, 1, 1}, {0, 0, 1}, {0, 0}}
};
const std::vector<uint32_t> cubeIdxs = {
    // -x
    0, 1, 2, 0, 2, 3,
    // -y
    4, 5, 6, 4, 6, 7,
    // -z
    8, 9, 10, 8, 10, 11,
    // +x
    12, 13, 14, 12, 14, 15,
    // +y
    16, 17, 18, 16, 18, 19,
    // +z
    20, 21, 22, 20, 22, 23
};

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

D3D12_RAYTRACING_INSTANCE_DESC* host_instanceDescs;
ComPtr<ID3D12Resource> dev_instanceDescs;
InstanceData* host_instanceDatas;
ComPtr<ID3D12Resource> dev_instanceDatas;

AcsHelper::GeometryWrapper quadGeoWrapper;
AcsHelper::GeometryWrapper cubeGeoWrapper;

ComPtr<ID3D12Resource> dev_tlas;
ComPtr<ID3D12Resource> dev_tlasUpdateScratchBuffer;

void updateTransforms();

void init(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    dev_vertBuffer.init((quadVerts.size() + cubeVerts.size()) * sizeof(Vertex));
    dev_idxBuffer.init(cubeIdxs.size() * sizeof(uint32_t));

    std::vector<AcsHelper::BlasBuildInputs> allBlasInputs;

    {
        AcsHelper::BlasBuildInputs blasInputs;
        blasInputs.host_verts = &quadVerts;
        blasInputs.dev_verts = &dev_vertBuffer;
        blasInputs.outGeoWrapper = &quadGeoWrapper;
        allBlasInputs.push_back(blasInputs);
    }

    {
        AcsHelper::BlasBuildInputs blasInputs;
        blasInputs.host_verts = &cubeVerts;
        blasInputs.host_idxs = &cubeIdxs;
        blasInputs.dev_verts = &dev_vertBuffer;
        blasInputs.dev_idxs = &dev_idxBuffer;
        blasInputs.outGeoWrapper = &cubeGeoWrapper;
        allBlasInputs.push_back(blasInputs);
    }

    AcsHelper::makeBlases(cmdList, toFreeList, allBlasInputs);

    BufferHelper::uavBarrier(cmdList, nullptr);

    dev_instanceDescs = BufferHelper::createBasicBuffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * MAX_INSTANCES,
                                                        &UPLOAD_HEAP,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ);
    dev_instanceDescs->Map(0, nullptr, reinterpret_cast<void**>(&host_instanceDescs));

    dev_instanceDatas = BufferHelper::createBasicBuffer(
        sizeof(InstanceData) * MAX_INSTANCES, &UPLOAD_HEAP, D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    dev_instanceDatas->Map(0, nullptr, reinterpret_cast<void**>(&host_instanceDatas));

    for (uint32_t i = 0; i < NUM_INSTANCES; ++i)
    {
        const bool isQuad = i > 0;

        host_instanceDescs[i] = {
            .InstanceID = i,
            .InstanceMask = 1,
            .AccelerationStructure = (isQuad ? quadGeoWrapper : cubeGeoWrapper).dev_blas->GetGPUVirtualAddress(),
        };

        host_instanceDatas[i] = {
            .vertBufferOffset =
                (uint32_t)((isQuad ? quadGeoWrapper : cubeGeoWrapper).vertBufferSection.offsetBytes / sizeof(Vertex)),
            .hasIdxs = !isQuad,
            .idxBufferByteOffset = isQuad ? 0 : cubeGeoWrapper.idxBufferSection.offsetBytes,
        };
    }

    updateTransforms();

    uint32_t updateScratchSize;

    AcsHelper::TlasBuildInputs inputs;
    inputs.dev_instanceDescs = dev_instanceDescs.Get();
    inputs.numInstances = NUM_INSTANCES;
    inputs.updateScratchSizePtr = &updateScratchSize;
    inputs.outTlas = &dev_tlas;

    AcsHelper::makeTlas(cmdList, toFreeList, inputs);

    auto desc = BASIC_BUFFER_DESC;
    desc.Width = updateScratchSize;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Renderer::device->CreateCommittedResource(&DEFAULT_HEAP,
                                              D3D12_HEAP_FLAG_NONE,
                                              &desc,
                                              D3D12_RESOURCE_STATE_COMMON,
                                              nullptr,
                                              IID_PPV_ARGS(&dev_tlasUpdateScratchBuffer));
}

void updateTransforms()
{
    using namespace DirectX;
    auto set = [](int idx, XMMATRIX mx)
    {
        auto* ptr = reinterpret_cast<XMFLOAT3X4*>(&host_instanceDescs[idx].Transform);
        XMStoreFloat3x4(ptr, mx);
    };

    auto time = static_cast<float>(GetTickCount64()) / 1000;

    auto cube = XMMatrixRotationRollPitchYaw(time / 2, time / 3, time / 5);
    cube *= XMMatrixTranslation(-1.5, 2, 2);
    set(0, cube);

    auto mirror = XMMatrixRotationX(-1.8f);
    mirror *= XMMatrixRotationY(XMScalarSinEst(time) / 8 + 1);
    mirror *= XMMatrixTranslation(2, 2, 2);
    set(1, mirror);

    auto floor = XMMatrixScaling(5, 5, 5);
    floor *= XMMatrixTranslation(0, 0, 2);
    set(2, floor);
}

void update(ID3D12GraphicsCommandList4* cmdList)
{
    updateTransforms();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {
        .DestAccelerationStructureData = dev_tlas->GetGPUVirtualAddress(),
        .Inputs = {
            .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
            .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE,
            .NumDescs = SceneManager::NUM_INSTANCES,
            .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
            .InstanceDescs = SceneManager::getDevInstanceDescs()->GetGPUVirtualAddress(),
        },
        .SourceAccelerationStructureData = dev_tlas->GetGPUVirtualAddress(),
        .ScratchAccelerationStructureData = dev_tlasUpdateScratchBuffer->GetGPUVirtualAddress(),
    };
    cmdList->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);

    BufferHelper::uavBarrier(cmdList, dev_tlas.Get());
}

ID3D12Resource* getDevInstanceDescs()
{
    return dev_instanceDescs.Get();
}

ID3D12Resource* getDevInstanceDatas()
{
    return dev_instanceDatas.Get();
}

ID3D12Resource* getDevTlas()
{
    return dev_tlas.Get();
}

ID3D12Resource* getDevVertBuffer()
{
    return dev_vertBuffer.getBuffer();
}

ID3D12Resource* getDevIdxBuffer()
{
    return dev_idxBuffer.getBuffer();
}

} // namespace SceneManager
