#include "acs_helper.h"

#include "renderer.h"
#include "buffer_helper.h"

namespace AcsHelper
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

static ComPtr<ID3D12Resource> sharedAcsScratchBuffer = nullptr;
static uint64_t sharedAsScratchSize = 0;

ComPtr<ID3D12Resource> makeAcsBuffer(uint32_t byteSize, D3D12_RESOURCE_STATES initialState)
{
    auto desc = BASIC_BUFFER_DESC;
    desc.Width = byteSize;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> buffer;
    Renderer::device->CreateCommittedResource(&DEFAULT_HEAP, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&buffer));
    return buffer;
}

struct AcsBuildInfo
{
    D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc; // optional, used only for BLAS
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo;
    ComPtr<ID3D12Resource>* outAcs;
};

void makeAccelerationStructures(ID3D12GraphicsCommandList4* cmdList,
                                ToFreeList* toFreeList,
                                const std::vector<AcsBuildInfo>& buildInfos)
{
    uint64_t maxScratchSize = 0;
    for (const auto& buildInfo : buildInfos)
    {
        maxScratchSize = std::max(buildInfo.prebuildInfo.ScratchDataSizeInBytes, maxScratchSize);
    }

    if (maxScratchSize > sharedAsScratchSize)
    {
        if (sharedAcsScratchBuffer)
        {
            toFreeList->push_back(sharedAcsScratchBuffer);
        }

        sharedAcsScratchBuffer = makeAcsBuffer(maxScratchSize, D3D12_RESOURCE_STATE_COMMON);
        sharedAsScratchSize = maxScratchSize;
    }

    for (uint32_t i = 0; i < buildInfos.size(); ++i)
    {
        const auto& buildInfo = buildInfos[i];

        *buildInfo.outAcs = makeAcsBuffer(buildInfo.prebuildInfo.ResultDataMaxSizeInBytes,
                                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {
            .DestAccelerationStructureData = (*buildInfo.outAcs)->GetGPUVirtualAddress(),
            .Inputs = buildInfo.inputs,
            .ScratchAccelerationStructureData = sharedAcsScratchBuffer->GetGPUVirtualAddress()
        };

        cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

        // It is the caller's responsibility to enforce a barrier for the last build if necessary.
        if (i < buildInfos.size() - 1)
        {
            BufferHelper::uavBarrier(cmdList, sharedAcsScratchBuffer.Get());
        }
    }
}

void makeBlasBuildInfo(AcsBuildInfo* buildInfo,
                       ComPtr<ID3D12Resource>* outBlas,
                       ID3D12Resource* vertBuffer,
                       uint32_t numVerts,
                       ID3D12Resource* idxBuffer = nullptr,
                       uint32_t numIdx = 0)
{
    buildInfo->geometryDesc = {
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
                .StrideInBytes = sizeof(Vertex),
            },
        },
    };

    buildInfo->inputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
        .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
        .NumDescs = 1,
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
        .pGeometryDescs = &buildInfo->geometryDesc,
    };

    Renderer::device->GetRaytracingAccelerationStructurePrebuildInfo(&buildInfo->inputs, &buildInfo->prebuildInfo);

    buildInfo->outAcs = outBlas;
}

void makeBuffersAndBlases(ID3D12GraphicsCommandList4* cmdList,
                          ToFreeList* toFreeList,
                          std::vector<BlasBuildInputs> allInputs)
{
    std::vector<AcsBuildInfo> buildInfos;
    buildInfos.reserve(allInputs.size());

    for (const auto& inputs : allInputs)
    {
        ComPtr<ID3D12Resource> dev_vertUploadBuffer = initAndCopyToUploadBuffer(*inputs.host_verts);
        ComPtr<ID3D12Resource> dev_idxUploadBuffer = nullptr;
        uint32_t numIdx = 0;
        if (inputs.host_idxs)
        {
            dev_idxUploadBuffer = initAndCopyToUploadBuffer(*inputs.host_idxs);
            numIdx = inputs.host_idxs->size();
        }

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

        // These won't be freed until after the frame is done being rendered, so we can safely push them to toFreeList
        // before building the BLAS.
        //
        // This should also keep the ComPtrs and associated resources alive for actually building the BLAS.
        toFreeList->push_back(dev_vertUploadBuffer);
        if (dev_idxUploadBuffer)
        {
            toFreeList->push_back(dev_idxUploadBuffer);
        }

        ID3D12Resource* dev_idxUploadBufferPtr = dev_idxUploadBuffer ? dev_idxUploadBuffer.Get() : nullptr;

        buildInfos.emplace_back();
        makeBlasBuildInfo(&buildInfos.back(),
                          &inputs.outGeoWrapper->dev_blas,
                          dev_vertUploadBuffer.Get(),
                          inputs.host_verts->size(),
                          dev_idxUploadBufferPtr,
                          numIdx);
    }

    makeAccelerationStructures(cmdList, toFreeList, buildInfos);
}

void makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList* toFreeList, TlasBuildInputs inputs)
{
    AcsBuildInfo buildInfo;

    buildInfo.inputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
        .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE,
        .NumDescs = inputs.numInstances,
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
        .InstanceDescs = inputs.dev_instanceDescs->GetGPUVirtualAddress()
    };

    Renderer::device->GetRaytracingAccelerationStructurePrebuildInfo(&buildInfo.inputs, &buildInfo.prebuildInfo);

    if (inputs.updateScratchSizePtr != nullptr)
    {
        *inputs.updateScratchSizePtr = buildInfo.prebuildInfo.UpdateScratchDataSizeInBytes;
    }

    buildInfo.outAcs = inputs.outTlas;

    makeAccelerationStructures(cmdList, toFreeList, { buildInfo });
}

} // namespace AcsHelper
