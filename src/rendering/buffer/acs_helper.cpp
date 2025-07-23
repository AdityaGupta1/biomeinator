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
    return BufferHelper::createBasicBuffer(
        sizeBytes, &DEFAULT_HEAP, initialState, { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
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
            toFreeList.pushResource(sharedAcsScratchBuffer, false);
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

        // The caller is responsible for enforcing a barrier for the last build if necessary.
        if (i < buildInfos.size() - 1)
        {
            BufferHelper::uavBarrier(cmdList, sharedAcsScratchBuffer.Get());
        }
    }
}

void makeBlasBuildInfo(AcsBuildInfo* buildInfo,
                       ComPtr<ID3D12Resource>* outBlas,
                       ManagedBufferSection vertsBufferSection,
                       ManagedBufferSection idxsBufferSection)
{
    const bool hasIdxs = (idxsBufferSection.sizeBytes > 0);

    buildInfo->geometryDesc = {
        .Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
        .Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE,

        .Triangles = {
            .Transform3x4 = 0,
            .IndexFormat = hasIdxs ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN,
            .VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT,
            .IndexCount = Util::convertByteSizeToCount<uint32_t>(idxsBufferSection.sizeBytes),
            .VertexCount = Util::convertByteSizeToCount<Vertex>(vertsBufferSection.sizeBytes),
            .IndexBuffer = hasIdxs ? idxsBufferSection.getBuffer()->getBufferGpuAddress() + idxsBufferSection.offsetBytes : 0,
            .VertexBuffer = {
                .StartAddress = vertsBufferSection.getBuffer()->getBufferGpuAddress() + vertsBufferSection.offsetBytes,
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

void makeBlases(ID3D12GraphicsCommandList4* cmdList,
                ToFreeList& toFreeList,
                const std::vector<BlasBuildInputs>& allInputs)
{
    ManagedBuffer vertsUploadBuffer{
        &UPLOAD_HEAP,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        false /*isResizable*/,
        true /*isMapped*/,
    };
    ManagedBuffer idxsUploadBuffer{
        &UPLOAD_HEAP,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        false /*isResizable*/,
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

    vertsUploadBuffer.init(vertBufferTotalSizeBytes);
    const bool anyHasIdxs = (idxBufferTotalSizeBytes > 0);
    if (anyHasIdxs)
    {
        idxsUploadBuffer.init(idxBufferTotalSizeBytes);
    }

    std::vector<AcsBuildInfo> buildInfos;
    buildInfos.reserve(allInputs.size());

    for (const auto& inputs : allInputs)
    {
        const ManagedBufferSection vertsUploadBufferSection =
            vertsUploadBuffer.copyFromHostVector(cmdList, toFreeList, *inputs.host_verts);

        if (inputs.dev_verts)
        {
            inputs.outGeoWrapper->vertsBufferSection = inputs.dev_verts->copyFromManagedBuffer(
                cmdList, toFreeList, vertsUploadBuffer, vertsUploadBufferSection);
        }

        ManagedBufferSection idxsUploadBufferSection = {};
        if (inputs.host_idxs)
        {
            idxsUploadBufferSection = idxsUploadBuffer.copyFromHostVector(cmdList, toFreeList, *inputs.host_idxs);

            if (inputs.dev_idxs)
            {
                inputs.outGeoWrapper->idxsBufferSection = inputs.dev_idxs->copyFromManagedBuffer(
                    cmdList, toFreeList, idxsUploadBuffer, idxsUploadBufferSection);
            }
        }

        buildInfos.emplace_back();
        makeBlasBuildInfo(&buildInfos.back(),
                          &inputs.outGeoWrapper->dev_blas,
                          vertsUploadBufferSection,
                          idxsUploadBufferSection);
    }

    makeAccelerationStructures(cmdList, toFreeList, buildInfos);

    toFreeList.pushManagedBuffer(&vertsUploadBuffer);
    if (anyHasIdxs)
    {
        toFreeList.pushManagedBuffer(&idxsUploadBuffer);
    }
}

void makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, const TlasBuildInputs& inputs)
{
    AcsBuildInfo buildInfo;

    const bool allowUpdates = (inputs.updateScratchSizePtr != nullptr);

    buildInfo.inputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
        .Flags = allowUpdates ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
                              : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
        .NumDescs = inputs.numInstances,
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
        .InstanceDescs = inputs.dev_instanceDescs->GetGPUVirtualAddress(),
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
