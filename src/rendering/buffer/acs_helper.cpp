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
#include "util/math.h"
#include "util/util.h"

namespace AcsHelper
{

static CommittedManagedBuffer sharedAcsScratchBuffer{
    &DEFAULT_HEAP,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    {
        .isResizable = true,
        .bufferCreationFlags = {
            .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        },
    }
};

struct AcsBuildInfo
{
    D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc; // optional, used only for BLAS
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo;

    ManagedBufferSection* outAcs;
};

static ReservedManagedBuffer sharedAcsBuffer{
    8ull * 1024 * 1024 * 1024, // 8 GB
    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
    {
        .isResizable = true,
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

        const size_t acsSizeBytes = MathUtil::roundUp(buildInfo.prebuildInfo.ResultDataMaxSizeInBytes,
                                                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        *buildInfo.outAcs = sharedAcsBuffer.findFreeSection(cmdList, &toFreeList, acsSizeBytes);

        const size_t scratchSizeBytes = MathUtil::roundUp(buildInfo.prebuildInfo.ScratchDataSizeInBytes,
                                                          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        ManagedBufferSection sharedAcsScratchSection =
            sharedAcsScratchBuffer.findFreeSection(cmdList, &toFreeList, scratchSizeBytes);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {
            .DestAccelerationStructureData = buildInfo.outAcs->getGpuVirtualAddress(),
            .Inputs = buildInfo.inputs,
            .ScratchAccelerationStructureData = sharedAcsScratchSection.getGpuVirtualAddress(),
        };

        toFreeList.pushManagedBufferSection(sharedAcsScratchSection);

        cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    }
}

static void makeBlasBuildInputs(AcsBuildInfo* buildInfo, const GeometryWrapper* geoWrapper, bool allowUpdate)
{
    const ManagedBufferSection vertsBufferSection = geoWrapper->vertsBufferSection;
    const ManagedBufferSection idxsBufferSection = geoWrapper->idxsBufferSection;
    const bool hasIdxs = (idxsBufferSection.sizeBytes > 0);

    buildInfo->geometryDesc = {
        .Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
        .Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE,

        .Triangles = {
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
        },
    };

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

    for (const auto& inputs : allInputs)
    {
        const ManagedBufferSection vertsUploadBufferSection =
            sharedVertsUploadBuffer.copyFromHostVector(cmdList, toFreeList, *inputs.host_verts);

        inputs.outGeoWrapper->vertsBufferSection =
            dev_verts->copyFromManagedBuffer(cmdList, toFreeList, sharedVertsUploadBuffer, vertsUploadBufferSection);

        toFreeList.pushManagedBufferSection(vertsUploadBufferSection);

        ManagedBufferSection idxsUploadBufferSection = {};
        if (inputs.host_idxs)
        {
            idxsUploadBufferSection = sharedIdxsUploadBuffer.copyFromHostVector(cmdList, toFreeList, *inputs.host_idxs);

            inputs.outGeoWrapper->idxsBufferSection =
                dev_idxs->copyFromManagedBuffer(cmdList, toFreeList, sharedIdxsUploadBuffer, idxsUploadBufferSection);

            toFreeList.pushManagedBufferSection(idxsUploadBufferSection);
        }

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

        const size_t scratchSizeBytes = MathUtil::roundUp(geoWrapper->updateScratchSizeBytes,
                                                          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        const ManagedBufferSection scratchSection =
            sharedAcsScratchBuffer.findFreeSection(cmdList, &toFreeList, scratchSizeBytes);

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
    sharedVertsUploadBuffer.reset();
    sharedIdxsUploadBuffer.reset();

    sharedAcsScratchBuffer.reset();
    sharedAcsBuffer.reset();
}

} // namespace AcsHelper
