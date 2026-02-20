/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

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
    sharedAcsBuffer.init(reservedGrowthChunkBytes);

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

static void makeBlasBuildInfo(AcsBuildInfo* buildInfo,
                              ManagedBufferSection* outBlas,
                              ManagedBufferSection vertsBufferSection,
                              ManagedBufferSection idxsBufferSection)
{
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

    buildInfo->inputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
        .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, // TODO: sacrifice some tracing perf for better building perf?
        .NumDescs = 1,
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
        .pGeometryDescs = &buildInfo->geometryDesc,
    };

    Renderer::getDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&buildInfo->inputs, &buildInfo->prebuildInfo);

    buildInfo->outAcs = outBlas;
}

void makeBlases(ID3D12GraphicsCommandList4* cmdList,
                ToFreeList& toFreeList,
                const std::vector<BlasBuildInputs>& allInputs)
{
    std::vector<AcsBuildInfo> buildInfos;
    buildInfos.reserve(allInputs.size());

    for (const auto& inputs : allInputs)
    {
        const ManagedBufferSection vertsUploadBufferSection =
            sharedVertsUploadBuffer.copyFromHostVector(cmdList, toFreeList, *inputs.host_verts);

        ASSERT(inputs.dev_verts != nullptr);
        inputs.outGeoWrapper->vertsBufferSection = inputs.dev_verts->copyFromManagedBuffer(
            cmdList, toFreeList, sharedVertsUploadBuffer, vertsUploadBufferSection);

        toFreeList.pushManagedBufferSection(vertsUploadBufferSection);

        ManagedBufferSection idxsUploadBufferSection = {};
        if (inputs.host_idxs)
        {
            idxsUploadBufferSection = sharedIdxsUploadBuffer.copyFromHostVector(cmdList, toFreeList, *inputs.host_idxs);

            ASSERT(inputs.dev_idxs != nullptr);
            inputs.outGeoWrapper->idxsBufferSection = inputs.dev_idxs->copyFromManagedBuffer(
                cmdList, toFreeList, sharedIdxsUploadBuffer, idxsUploadBufferSection);

            toFreeList.pushManagedBufferSection(idxsUploadBufferSection);
        }

        buildInfos.emplace_back();
        makeBlasBuildInfo(&buildInfos.back(),
                          &inputs.outGeoWrapper->blasBufferSection,
                          vertsUploadBufferSection,
                          idxsUploadBufferSection);
    }

    makeAccelerationStructures(cmdList, toFreeList, buildInfos);
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
}

} // namespace AcsHelper
