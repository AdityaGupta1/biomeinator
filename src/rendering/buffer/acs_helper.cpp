// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "acs_helper.h"

#include "buffer_helper.h"
#include "committed_managed_buffer.h"
#include "reserved_managed_buffer.h"
#include "debug.h"
#include "managed_buffer.h"
#include "to_free_list.h"
#include "rendering/renderer.h"
#include "util/util.h"

#include <cstring>

namespace AcsHelper
{

static CommittedManagedBuffer sharedAcsScratchBuffer{
    &DEFAULT_HEAP,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    {
        .isResizable = true,
        .alignmentBytes = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT,
        .bufferCreationFlags = {
            .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        },
    }
};

struct AcsBuildInfo
{
    D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc; // optional, used only for BLAS
    // Referenced by geometryDesc for OMM-linked BLASes, whose union holds pointers instead of
    // an inline triangles desc
    D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC trianglesDesc;
    D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC ommLinkageDesc;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo;

    ManagedBufferSection* outAcs;
};

// The single OMM Array shared by all OMM-linked BLASes; see buildOmmArray()
static D3D12_GPU_VIRTUAL_ADDRESS ommArrayGpuVa = 0;

static ReservedManagedBuffer sharedAcsBuffer{
    8ull * 1024 * 1024 * 1024, // 8 GB
    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
    {
        .isResizable = true,
        .alignmentBytes = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT,
        .bufferCreationFlags = {
            .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        },
    }
};

static CommittedManagedBuffer sharedVertsUploadBuffer{
    &UPLOAD_HEAP,
    D3D12_RESOURCE_STATE_GENERIC_READ,
    {
        .isResizable = true,
        .isMapped = true,
    },
};
static CommittedManagedBuffer sharedIdxsUploadBuffer{
    &UPLOAD_HEAP,
    D3D12_RESOURCE_STATE_GENERIC_READ,
    {
        .isResizable = true,
        .isMapped = true,
    },
};

void init()
{
    sharedAcsBuffer.setName(L"sharedAcsBuffer");
    sharedAcsBuffer.init();

    sharedVertsUploadBuffer.setName(L"sharedVertsUploadBuffer");
    sharedVertsUploadBuffer.init(1 << 14 /*bytes*/);

    sharedIdxsUploadBuffer.setName(L"sharedIdxsUploadBuffer");
    sharedIdxsUploadBuffer.init(1 << 12 /*bytes*/);

    sharedAcsScratchBuffer.setName(L"sharedAcsScratchBuffer");
    sharedAcsScratchBuffer.init(1 << 14 /*bytes*/);
}

static void makeAccelerationStructures(ID3D12GraphicsCommandList4* cmdList,
                                       ToFreeList& toFreeList,
                                       const std::vector<AcsBuildInfo>& buildInfos)
{
    for (size_t i = 0; i < buildInfos.size(); ++i)
    {
        const auto& buildInfo = buildInfos[i];

        *buildInfo.outAcs =
            sharedAcsBuffer.findFreeSection(cmdList, &toFreeList, buildInfo.prebuildInfo.ResultDataMaxSizeInBytes);

        ManagedBufferSection sharedAcsScratchSection =
            sharedAcsScratchBuffer.findFreeSection(cmdList, &toFreeList, buildInfo.prebuildInfo.ScratchDataSizeInBytes);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {
            .DestAccelerationStructureData = buildInfo.outAcs->getGpuVirtualAddress(),
            .Inputs = buildInfo.inputs,
            .ScratchAccelerationStructureData = sharedAcsScratchSection.getGpuVirtualAddress(),
        };

        toFreeList.pushManagedBufferSection(sharedAcsScratchSection);

        cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    }
}

void buildOmmArray(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, const OmmArrayBuildInputs& inputs)
{
    ASSERT(ommArrayGpuVa == 0); // single OMM Array; makeBlasBuildInputs links against its VA
    ASSERT(inputs.host_ommData != nullptr && !inputs.host_ommData->empty());
    ASSERT(inputs.host_ommDescs != nullptr && !inputs.host_ommDescs->empty());

    // Pack the raw bitmask data and per-OMM descs into one device buffer. The input data VA
    // must be 128-byte aligned, so it sits at offset 0 of a dedicated resource.
    const size_t dataSizeBytes = inputs.host_ommData->size();
    const size_t descsOffsetBytes = MathUtil::roundUp(dataSizeBytes, sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC));
    const size_t descsSizeBytes = inputs.host_ommDescs->size() * sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC);
    const size_t totalSizeBytes = descsOffsetBytes + descsSizeBytes;

    ComPtr<ID3D12Resource> uploadBuffer = BufferHelper::createBasicBuffer(totalSizeBytes, &UPLOAD_HEAP);
    uint8_t* host_mapped = nullptr;
    CHECK_HRESULT(uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&host_mapped)));
    std::memcpy(host_mapped, inputs.host_ommData->data(), dataSizeBytes);
    std::memcpy(host_mapped + descsOffsetBytes, inputs.host_ommDescs->data(), descsSizeBytes);
    uploadBuffer->Unmap(0, nullptr);

    ComPtr<ID3D12Resource> inputBuffer =
        BufferHelper::createBasicBuffer(totalSizeBytes, &DEFAULT_HEAP, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(inputBuffer.Get(), 0, uploadBuffer.Get(), 0, totalSizeBytes);
    BufferHelper::stateTransitionResourceBarrier(cmdList,
                                                 inputBuffer.Get(),
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const D3D12_GPU_VIRTUAL_ADDRESS inputVa = inputBuffer->GetGPUVirtualAddress();
    const D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC ommArrayDesc = {
        .NumOmmHistogramEntries = 1,
        .pOmmHistogram = &inputs.histogram,
        .InputBuffer = inputVa,
        .PerOmmDescs = {
            .StartAddress = inputVa + descsOffsetBytes,
            .StrideInBytes = sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC),
        },
    };

    AcsBuildInfo buildInfo;
    buildInfo.inputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY,
        .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE,
        .NumDescs = 1,
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
        .pOpacityMicromapArrayDesc = &ommArrayDesc,
    };

    Renderer::getDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&buildInfo.inputs, &buildInfo.prebuildInfo);

    buildInfo.outAcs = inputs.outOmmArray;

    makeAccelerationStructures(cmdList, toFreeList, { buildInfo });

    // Later BLAS builds dereference the OMM Array from the same buffer
    BufferHelper::uavBarrier(cmdList, sharedAcsBuffer.getBuffer());

    ommArrayGpuVa = inputs.outOmmArray->getGpuVirtualAddress();

    toFreeList.pushResource(uploadBuffer);
    toFreeList.pushResource(inputBuffer);
}

static void makeBlasBuildInputs(AcsBuildInfo* buildInfo, const GeometryWrapper* geoWrapper, bool allowUpdate)
{
    const ManagedBufferSection vertsBufferSection = geoWrapper->vertsBufferSection;
    const ManagedBufferSection idxsBufferSection = geoWrapper->idxsBufferSection;
    const ManagedBufferSection ommIdxsBufferSection = geoWrapper->ommIdxsBufferSection;
    const bool hasIdxs = (idxsBufferSection.sizeBytes > 0);
    const bool hasOmms = (ommIdxsBufferSection.sizeBytes > 0);

    buildInfo->trianglesDesc = {
        .Transform3x4 = 0,
        .IndexFormat = hasIdxs ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN,
        .VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT,
        .IndexCount = Util::convertByteSizeToCount<uint32_t>(idxsBufferSection.sizeBytes),
        .VertexCount = Util::convertByteSizeToCount<Vertex>(vertsBufferSection.sizeBytes),
        .IndexBuffer = hasIdxs ? idxsBufferSection.getBuffer()->getGpuVirtualAddress() + idxsBufferSection.offsetBytes : 0,
        .VertexBuffer = {
            .StartAddress = vertsBufferSection.getBuffer()->getGpuVirtualAddress() + vertsBufferSection.offsetBytes,
            .StrideInBytes = sizeof(Vertex),
        },
    };

    if (hasOmms)
    {
        ASSERT(ommArrayGpuVa != 0);
        buildInfo->ommLinkageDesc = {
            .OpacityMicromapIndexBuffer = {
                .StartAddress = ommIdxsBufferSection.getBuffer()->getGpuVirtualAddress() + ommIdxsBufferSection.offsetBytes,
                .StrideInBytes = sizeof(uint16_t),
            },
            .OpacityMicromapIndexFormat = DXGI_FORMAT_R16_UINT,
            .OpacityMicromapBaseLocation = 0,
            .OpacityMicromapArray = ommArrayGpuVa,
        };

        buildInfo->geometryDesc = {
            .Type = D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES,
            .Flags = geoWrapper->geometryFlags,

            .OmmTriangles = {
                .pTriangles = &buildInfo->trianglesDesc,
                .pOmmLinkage = &buildInfo->ommLinkageDesc,
            },
        };
    }
    else
    {
        buildInfo->geometryDesc = {
            .Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
            .Flags = geoWrapper->geometryFlags,

            .Triangles = buildInfo->trianglesDesc,
        };
    }

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (allowUpdate)
    {
        // ALLOW_UPDATE must also be passed to GetRaytracingAccelerationStructurePrebuildInfo,
        // or UpdateScratchDataSizeInBytes comes back 0
        buildFlags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    }

    buildInfo->inputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
        .Flags = buildFlags,
        .NumDescs = 1,
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
        .pGeometryDescs = &buildInfo->geometryDesc,
    };
}

static void makeBlasBuildInfo(AcsBuildInfo* buildInfo, GeometryWrapper* geoWrapper, bool allowUpdate)
{
    makeBlasBuildInputs(buildInfo, geoWrapper, allowUpdate);

    Renderer::getDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&buildInfo->inputs, &buildInfo->prebuildInfo);

    geoWrapper->updateScratchSizeBytes = allowUpdate ? buildInfo->prebuildInfo.UpdateScratchDataSizeInBytes : 0;

    buildInfo->outAcs = &geoWrapper->blasBufferSection;
}

void makeBlases(ID3D12GraphicsCommandList4* cmdList,
                ToFreeList& toFreeList,
                ManagedBuffer* dev_verts,
                ManagedBuffer* dev_idxs,
                const std::vector<BlasBuildInputs>& allInputs)
{
    std::vector<AcsBuildInfo> buildInfos;
    buildInfos.reserve(allInputs.size());

    dev_verts->beginBatchCopy(cmdList);
    dev_idxs->beginBatchCopy(cmdList);

    const auto uploadIdxsSection = [&](const auto& host_idxs)
    {
        const ManagedBufferSection uploadBufferSection =
            sharedIdxsUploadBuffer.copyFromHostVector(cmdList, toFreeList, host_idxs);

        const ManagedBufferSection devBufferSection =
            dev_idxs->copyFromManagedBuffer(cmdList, toFreeList, sharedIdxsUploadBuffer, uploadBufferSection);

        toFreeList.pushManagedBufferSection(uploadBufferSection);
        return devBufferSection;
    };

    for (const auto& inputs : allInputs)
    {
        const ManagedBufferSection vertsUploadBufferSection =
            sharedVertsUploadBuffer.copyFromHostVector(cmdList, toFreeList, *inputs.host_verts);

        inputs.outGeoWrapper->vertsBufferSection =
            dev_verts->copyFromManagedBuffer(cmdList, toFreeList, sharedVertsUploadBuffer, vertsUploadBufferSection);

        toFreeList.pushManagedBufferSection(vertsUploadBufferSection);

        if (inputs.host_idxs)
        {
            inputs.outGeoWrapper->idxsBufferSection = uploadIdxsSection(*inputs.host_idxs);
        }

        if (inputs.host_ommIdxs)
        {
            inputs.outGeoWrapper->ommIdxsBufferSection = uploadIdxsSection(*inputs.host_ommIdxs);
        }

        inputs.outGeoWrapper->geometryFlags = inputs.isOpaque
            ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
            : D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;

        buildInfos.emplace_back();
        makeBlasBuildInfo(&buildInfos.back(), inputs.outGeoWrapper, inputs.allowUpdate);
    }

    dev_verts->endBatchCopy(cmdList);
    dev_idxs->endBatchCopy(cmdList);

    makeAccelerationStructures(cmdList, toFreeList, buildInfos);
}

void updateBlases(ID3D12GraphicsCommandList4* cmdList,
                  ToFreeList& toFreeList,
                  const std::vector<GeometryWrapper*>& geoWrappers)
{
    if (geoWrappers.empty())
    {
        return;
    }

    // Last frame's DispatchRays read these BLASes and the in-place refit writes the same
    // memory; the buffer lives permanently in the AS state, so ordering is UAV-barrier-only.
    // The existing barrier in makeTlas only covers this frame's writes -> TLAS read, not
    // last frame's read -> this frame's write.
    BufferHelper::uavBarrier(cmdList, sharedAcsBuffer.getBuffer());

    for (GeometryWrapper* const geoWrapper : geoWrappers)
    {
        AcsBuildInfo buildInfo;
        makeBlasBuildInputs(&buildInfo, geoWrapper, true /*allowUpdate*/);
        // update flags must match the original build's flags aside from PERFORM_UPDATE, and
        // ALLOW_UPDATE must stay set or no further updates are allowed
        buildInfo.inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

        const ManagedBufferSection scratchSection =
            sharedAcsScratchBuffer.findFreeSection(cmdList, &toFreeList, geoWrapper->updateScratchSizeBytes);

        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {
            .DestAccelerationStructureData = geoWrapper->blasBufferSection.getGpuVirtualAddress(),
            .Inputs = buildInfo.inputs,
            .SourceAccelerationStructureData = geoWrapper->blasBufferSection.getGpuVirtualAddress(),
            .ScratchAccelerationStructureData = scratchSection.getGpuVirtualAddress(),
        };

        toFreeList.pushManagedBufferSection(scratchSection);

        cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    }
}

void makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, const TlasBuildInputs& inputs)
{
    AcsBuildInfo buildInfo;

    const bool allowUpdate = (inputs.updateScratchSizePtr != nullptr);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (allowUpdate)
    {
        buildFlags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    }

    BufferHelper::uavBarrier(cmdList, sharedAcsBuffer.getBuffer()); // ensure BLAS writes are completed before building TLAS

    buildInfo.inputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
        .Flags = buildFlags,
        .NumDescs = inputs.numInstances,
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
        .InstanceDescs = inputs.dev_instanceDescs->GetGPUVirtualAddress(),
    };

    Renderer::getDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&buildInfo.inputs, &buildInfo.prebuildInfo);

    if (inputs.updateScratchSizePtr != nullptr)
    {
        *inputs.updateScratchSizePtr = buildInfo.prebuildInfo.UpdateScratchDataSizeInBytes; // TODO: this isn't actually used anywhere yet lol
    }

    buildInfo.outAcs = inputs.outTlas;

    makeAccelerationStructures(cmdList, toFreeList, { buildInfo });
}

void reset()
{
    ommArrayGpuVa = 0;

    sharedVertsUploadBuffer.reset();
    sharedIdxsUploadBuffer.reset();

    sharedAcsScratchBuffer.reset();
    sharedAcsBuffer.reset();
}

} // namespace AcsHelper
