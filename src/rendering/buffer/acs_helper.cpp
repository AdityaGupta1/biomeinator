#include "acs_helper.h"

#include "buffer_helper.h"
#include "managed_buffer.h"
#include "to_free_list.h"
#include "rendering/renderer.h"
#include "util/util.h"

namespace AcsHelper
{

static ComPtr<ID3D12Resource> sharedAcsScratchBuffer = nullptr;
static uint64_t sharedAsScratchSize = 0;

ComPtr<ID3D12Resource> makeAcsBuffer(uint32_t sizeBytes, D3D12_RESOURCE_STATES initialState)
{
    auto desc = BASIC_BUFFER_DESC;
    desc.Width = sizeBytes;
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
                                ToFreeList& toFreeList,
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
            toFreeList.pushResource(sharedAcsScratchBuffer);
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
                       const ManagedBuffer& dev_vertUploadBuffer,
                       ManagedBufferSection vertBufferSection,
                       const ManagedBuffer& dev_idxUploadBuffer,
                       ManagedBufferSection idxBufferSection)
{
    const bool hasIdxs = (idxBufferSection.sizeBytes > 0);

    buildInfo->geometryDesc = {
        .Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
        .Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE,

        .Triangles = {
            .Transform3x4 = 0,
            .IndexFormat = hasIdxs ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN,
            .VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT,
            .IndexCount = Util::convertByteSizeToCount<uint32_t>(idxBufferSection.sizeBytes),
            .VertexCount = Util::convertByteSizeToCount<Vertex>(vertBufferSection.sizeBytes),
            .IndexBuffer = hasIdxs ? dev_idxUploadBuffer.getBufferGpuAddress() + idxBufferSection.offsetBytes : 0,
            .VertexBuffer = {
                .StartAddress = dev_vertUploadBuffer.getBufferGpuAddress() + vertBufferSection.offsetBytes,
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

void makeBlases(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, std::vector<BlasBuildInputs> allInputs)
{
    ManagedBuffer dev_vertUploadBuffer{
        &UPLOAD_HEAP,
        D3D12_HEAP_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        true /*isMapped*/,
    };
    ManagedBuffer dev_idxUploadBuffer{
        &UPLOAD_HEAP,
        D3D12_HEAP_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        true /*isMapped*/,
    };

    uint32_t vertBufferTotalSizeBytes = 0;
    uint32_t idxBufferTotalSizeBytes = 0;
    for (const auto& inputs : allInputs)
    {
        vertBufferTotalSizeBytes += Util::getVectorSizeBytes(*inputs.host_verts);
        if (inputs.host_idxs)
        {
            idxBufferTotalSizeBytes += Util::getVectorSizeBytes(*inputs.host_idxs);
        }
    }

    dev_vertUploadBuffer.init(vertBufferTotalSizeBytes);
    const bool anyHasIdxs = (idxBufferTotalSizeBytes > 0);
    if (anyHasIdxs)
    {
        dev_idxUploadBuffer.init(idxBufferTotalSizeBytes);
    }

    std::vector<AcsBuildInfo> buildInfos;
    buildInfos.reserve(allInputs.size());

    for (const auto& inputs : allInputs)
    {
        const ManagedBufferSection dev_vertUploadBufferSection =
            dev_vertUploadBuffer.copyFromHostVector(cmdList, *inputs.host_verts);

        if (inputs.dev_verts)
        {
            inputs.outGeoWrapper->vertBufferSection = inputs.dev_verts->copyFromManagedBuffer(
                cmdList, dev_vertUploadBuffer, dev_vertUploadBufferSection);
        }

        ManagedBufferSection dev_idxUploadBufferSection = {};
        if (inputs.host_idxs)
        {
            dev_idxUploadBufferSection = dev_idxUploadBuffer.copyFromHostVector(cmdList, *inputs.host_idxs);

            if (inputs.dev_idxs)
            {
                inputs.outGeoWrapper->idxBufferSection =
                    inputs.dev_idxs->copyFromManagedBuffer(cmdList, dev_idxUploadBuffer, dev_idxUploadBufferSection);
            }
        }

        buildInfos.emplace_back();
        makeBlasBuildInfo(&buildInfos.back(),
                          &inputs.outGeoWrapper->dev_blas,
                          dev_vertUploadBuffer,
                          dev_vertUploadBufferSection,
                          dev_idxUploadBuffer,
                          dev_idxUploadBufferSection);
    }

    makeAccelerationStructures(cmdList, toFreeList, buildInfos);

    toFreeList.pushManagedBuffer(&dev_vertUploadBuffer);
    if (anyHasIdxs)
    {
        toFreeList.pushManagedBuffer(&dev_idxUploadBuffer);
    }
}

void makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, TlasBuildInputs inputs)
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
