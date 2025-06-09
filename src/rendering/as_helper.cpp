#include "as_helper.h"

#include "renderer.h"

namespace AsHelper
{

template<class T>
ComPtr<ID3D12Resource> initAndCopyToUploadBuffer(const std::vector<T>& host_vector)
{
    const size_t hostDataByteSize = sizeof(T) * host_vector.size();

    auto desc = BASIC_BUFFER_DESC;
    desc.Width = hostDataByteSize;

    ComPtr<ID3D12Resource> res;
    Renderer::device->CreateCommittedResource(&UPLOAD_HEAP, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&res));

    void* ptr;
    res->Map(0, nullptr, &ptr);
    memcpy(ptr, host_vector.data(), hostDataByteSize);
    res->Unmap(0, nullptr);

    return res;
}

static ComPtr<ID3D12Resource> sharedAsScratchBuffer = nullptr;
static uint64_t sharedAsScratchSize = 0;

ComPtr<ID3D12Resource> makeAsBuffer(uint32_t byteSize, D3D12_RESOURCE_STATES initialState)
{
    auto desc = BASIC_BUFFER_DESC;
    desc.Width = byteSize;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> buffer;
    Renderer::device->CreateCommittedResource(&DEFAULT_HEAP, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&buffer));
    return buffer;
}

ComPtr<ID3D12Resource> makeAccelerationStructure(ID3D12GraphicsCommandList4* cmdList,
                                                 ToFreeList* toFreeList,
                                                 const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs,
                                                 uint64_t* updateScratchSize = nullptr)
{
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo;
    Renderer::device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);
    if (updateScratchSize)
    {
        *updateScratchSize = prebuildInfo.UpdateScratchDataSizeInBytes;
    }

    if (prebuildInfo.ScratchDataSizeInBytes > sharedAsScratchSize)
    {
        if (sharedAsScratchBuffer)
        {
            toFreeList->push_back(sharedAsScratchBuffer);
        }

        sharedAsScratchBuffer = makeAsBuffer(prebuildInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON);
    }

    ComPtr<ID3D12Resource> asBuffer = makeAsBuffer(prebuildInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {
        .DestAccelerationStructureData = asBuffer->GetGPUVirtualAddress(),
        .Inputs = inputs,
        .ScratchAccelerationStructureData = sharedAsScratchBuffer->GetGPUVirtualAddress()
    };

    cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    return asBuffer;
}

ComPtr<ID3D12Resource> makeBlas(ID3D12GraphicsCommandList4* cmdList,
                                ToFreeList* toFreeList,
                                ID3D12Resource* vertBuffer,
                                uint32_t numVerts,
                                ID3D12Resource* idxBuffer = nullptr,
                                uint32_t numIdx = 0)
{
    D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {
    .Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
    .Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE,

    .Triangles = {
        .Transform3x4 = 0,
        .IndexFormat = idxBuffer ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN,
        .VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT,
        .IndexCount = numIdx,
        .VertexCount = numVerts,
        .IndexBuffer = idxBuffer ? idxBuffer->GetGPUVirtualAddress() : 0,
        .VertexBuffer = {
            .StartAddress = vertBuffer->GetGPUVirtualAddress(),
            .StrideInBytes = sizeof(Vertex)
        }
    }
    };

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
        .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
        .NumDescs = 1,
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
        .pGeometryDescs = &geometryDesc
    };

    return makeAccelerationStructure(cmdList, toFreeList, inputs);
}

void makeBuffersAndBlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList* toFreeList, BlasInputs inputs)
{
    ComPtr<ID3D12Resource> dev_vertUploadBuffer = initAndCopyToUploadBuffer(*inputs.host_verts);
    ComPtr<ID3D12Resource> dev_idxUploadBuffer = nullptr;
    uint32_t numIdx = 0;
    if (inputs.host_idxs)
    {
        dev_idxUploadBuffer = initAndCopyToUploadBuffer(*inputs.host_idxs);
        numIdx = inputs.host_idxs->size();
    }

    ID3D12Resource* dev_idxUploadBufferPtr = dev_idxUploadBuffer ? dev_idxUploadBuffer.Get() : nullptr;
    inputs.outGeoWrapper->dev_blas =
        makeBlas(cmdList, toFreeList, dev_vertUploadBuffer.Get(), inputs.host_verts->size(), dev_idxUploadBufferPtr, numIdx);

    if (inputs.dev_managedVertBuffer)
    {
        inputs.outGeoWrapper->vertBufferSection = inputs.dev_managedVertBuffer->copyFromUploadHeap(
            cmdList, dev_vertUploadBuffer.Get(), inputs.host_verts->size() * sizeof(Vertex));
    }

    if (inputs.dev_managedIdxBuffer)
    {
        inputs.outGeoWrapper->idxBufferSection = inputs.dev_managedIdxBuffer->copyFromUploadHeap(
            cmdList, dev_idxUploadBuffer.Get(), inputs.host_idxs->size() * sizeof(uint32_t));
    }

    toFreeList->push_back(dev_vertUploadBuffer);
    if (dev_idxUploadBuffer)
    {
        toFreeList->push_back(dev_idxUploadBuffer);
    }
}

ComPtr<ID3D12Resource> makeTLAS(ID3D12GraphicsCommandList4* cmdList,
                                ToFreeList* toFreeList,
                                ID3D12Resource* dev_instanceDescs,
                                uint32_t numInstances,
                                uint64_t* updateScratchSize)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
        .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE,
        .NumDescs = numInstances,
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
        .InstanceDescs = dev_instanceDescs->GetGPUVirtualAddress()
    };

    return makeAccelerationStructure(cmdList, toFreeList, inputs, updateScratchSize);
}

} // namespace AsHelper
