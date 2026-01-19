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
#include "managed_buffer.h"
#include "to_free_list.h"
#include "rendering/renderer.h"
#include "util/util.h"

namespace AcsHelper
{

static ComPtr<ID3D12Resource> sharedAcsScratchBuffer = nullptr;
static uint64_t sharedAsScratchSize = 0;

struct AcsBuildInfo
{
    D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc; // optional, used only for BLAS
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo;
    ComPtr<ID3D12Resource>* outAcs;
};

static void makeAccelerationStructures(ID3D12GraphicsCommandList4* cmdList,
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

        sharedAcsScratchBuffer = BufferHelper::createBasicBuffer(
            maxScratchSize, &DEFAULT_HEAP, { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
        sharedAcsScratchBuffer->SetName(L"sharedAcsScratchBuffer");
        sharedAsScratchSize = maxScratchSize;
    }

    for (uint32_t i = 0; i < buildInfos.size(); ++i)
    {
        const auto& buildInfo = buildInfos[i];

        *buildInfo.outAcs =
            BufferHelper::createBasicBuffer(buildInfo.prebuildInfo.ResultDataMaxSizeInBytes,
                                            &DEFAULT_HEAP,
                                            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                                            { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
        (*buildInfo.outAcs)->SetName(L"buildInfo.outAcs");

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

static void makeBlasBuildInfo(AcsBuildInfo* buildInfo,
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

static ManagedBuffer sharedVertsUploadBuffer{
    &UPLOAD_HEAP,
    D3D12_RESOURCE_STATE_GENERIC_READ,
    {
        .isResizable = true,
        .isMapped = true,
    },
};
static ManagedBuffer sharedIdxsUploadBuffer{
    &UPLOAD_HEAP,
    D3D12_RESOURCE_STATE_GENERIC_READ,
    {
        .isResizable = true,
        .isMapped = true,
    },
};

void init()
{
    sharedVertsUploadBuffer.setName(L"sharedVertsUploadBuffer");
    sharedVertsUploadBuffer.init(1 << 14 /*bytes*/);

    sharedIdxsUploadBuffer.setName(L"sharedIdxsUploadBuffer");
    sharedIdxsUploadBuffer.init(1 << 12 /*bytes*/);
}

void makeBlases(ID3D12GraphicsCommandList4* cmdList,
                ToFreeList& toFreeList,
                const std::vector<BlasBuildInputs>& allInputs)
{
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

    std::vector<AcsBuildInfo> buildInfos;
    buildInfos.reserve(allInputs.size());

    for (const auto& inputs : allInputs)
    {
        const ManagedBufferSection vertsUploadBufferSection =
            sharedVertsUploadBuffer.copyFromHostVector(cmdList, toFreeList, *inputs.host_verts);

        if (inputs.dev_verts)
        {
            inputs.outGeoWrapper->vertsBufferSection = inputs.dev_verts->copyFromManagedBuffer(
                cmdList, toFreeList, sharedVertsUploadBuffer, vertsUploadBufferSection);
        }

        toFreeList.pushManagedBufferSection(vertsUploadBufferSection);

        ManagedBufferSection idxsUploadBufferSection = {};
        if (inputs.host_idxs)
        {
            idxsUploadBufferSection = sharedIdxsUploadBuffer.copyFromHostVector(cmdList, toFreeList, *inputs.host_idxs);

            if (inputs.dev_idxs)
            {
                inputs.outGeoWrapper->idxsBufferSection = inputs.dev_idxs->copyFromManagedBuffer(
                    cmdList, toFreeList, sharedIdxsUploadBuffer, idxsUploadBufferSection);
            }

            toFreeList.pushManagedBufferSection(idxsUploadBufferSection);
        }

        buildInfos.emplace_back();
        makeBlasBuildInfo(&buildInfos.back(),
                          &inputs.outGeoWrapper->dev_blas,
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

    Renderer::device->GetRaytracingAccelerationStructurePrebuildInfo(&buildInfo.inputs, &buildInfo.prebuildInfo);

    if (inputs.updateScratchSizePtr != nullptr)
    {
        *inputs.updateScratchSizePtr = buildInfo.prebuildInfo.UpdateScratchDataSizeInBytes;
    }

    buildInfo.outAcs = inputs.outTlas;

    makeAccelerationStructures(cmdList, toFreeList, { buildInfo });
}

void reset()
{
    sharedVertsUploadBuffer.reset();
    sharedIdxsUploadBuffer.reset();

    sharedAcsScratchBuffer.Reset();
}

} // namespace AcsHelper
