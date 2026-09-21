// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "area_light_compactor.h"

#include "buffer/buffer_helper.h"
#include "buffer/committed_managed_buffer.h"
#include "buffer/to_free_list.h"
#include "common/common_registers.h"
#include "common/common_settings.h"
#include "renderer/pipeline_builder.h"
#include "renderer/renderer_internal.h"
#include "renderer/shaders.h"
#include "util/util.h"

#include <array>

namespace AreaLightCompactor
{

using Renderer::makeParam; // for MAKE_PARAM

namespace
{

enum class AreaLightCompactParam
{
    CONSTANTS,

    RANGES,
    SRC,

    DST,

    COUNT
};

#define AREA_LIGHT_COMPACT_PARAM_IDX(name) static_cast<uint32_t>(AreaLightCompactParam::name)

struct AreaLightCompactConstants
{
    uint32_t numRanges;
    uint32_t numElements;
};

ComPtr<ID3D12RootSignature> rootSig{ nullptr };
ComPtr<ID3D12PipelineState> pso{ nullptr };

CommittedManagedBuffer rangesUploadBuffer{
    &UPLOAD_HEAP,
    D3D12_RESOURCE_STATE_GENERIC_READ,
    {
        .isResizable = true,
        .isMapped = true,
    },
};

// The compacted array is gathered into here and copied back, since the sampling structure's
// own buffer is not UAV-capable and an in-place gather could not be ordered anyway
CommittedManagedBuffer scratchBuffer{
    &DEFAULT_HEAP,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    {
        .isResizable = true,
        .alignmentBytes = sizeof(uint32_t),
        .bufferCreationFlags = {
            .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        },
    },
};

} // namespace

void init()
{
    std::array<D3D12_ROOT_PARAMETER1, AREA_LIGHT_COMPACT_PARAM_IDX(COUNT)> params;
    params[AREA_LIGHT_COMPACT_PARAM_IDX(CONSTANTS)] = {
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
        .Constants = {
            .ShaderRegister = AREA_LIGHT_COMPACT_REGISTER_CONSTANTS,
            .RegisterSpace = AREA_LIGHT_COMPACT_REGISTER_SPACE,
            .Num32BitValues = sizeof(AreaLightCompactConstants) / 4,
        },
    };
    params[AREA_LIGHT_COMPACT_PARAM_IDX(RANGES)] = MAKE_PARAM(SRV, AREA_LIGHT_COMPACT, RANGES);
    params[AREA_LIGHT_COMPACT_PARAM_IDX(SRC)] = MAKE_PARAM(SRV, AREA_LIGHT_COMPACT, SRC);
    params[AREA_LIGHT_COMPACT_PARAM_IDX(DST)] = MAKE_PARAM(UAV, AREA_LIGHT_COMPACT, DST);

    Renderer::serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                              nullptr, 0, rootSig);
    rootSig->SetName(L"areaLightCompactRootSig");

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.CS = makeShaderBytecode(getShader("area_light_compact_cs"));
    CHECK_HRESULT(Renderer::getDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)));
    pso->SetName(L"areaLightCompactPso");

    rangesUploadBuffer.setName(L"areaLightCompactRangesUploadBuffer");
    rangesUploadBuffer.init(1 << 16 /*bytes*/);

    // Room for a full rewrite of the sampling structure at its pre-sized capacity in each
    // frame in flight, since a section stays reserved until its frame retires; growing here
    // would be a copy and a release in the middle of a chunk-unload frame
    scratchBuffer.setName(L"areaLightCompactScratchBuffer");
    scratchBuffer.init((1ull << 21) * sizeof(uint32_t) * Renderer::NUM_FRAMES_IN_FLIGHT);
}

void dispatch(ID3D12GraphicsCommandList4* const cmdList,
              ToFreeList& toFreeList,
              ID3D12Resource* const samplingStructure,
              const uint32_t firstElement,
              const uint32_t numElements,
              const std::vector<AreaLightCompactRange>& ranges)
{
    if (numElements == 0)
    {
        return;
    }
    ASSERT(!ranges.empty());

    const ManagedBufferSection rangesSection = rangesUploadBuffer.copyFromHostVector(cmdList, toFreeList, ranges);
    toFreeList.pushManagedBufferSection(rangesSection);

    const size_t sizeBytes = static_cast<size_t>(numElements) * sizeof(uint32_t);
    const ManagedBufferSection scratchSection = scratchBuffer.findFreeSection(cmdList, &toFreeList, sizeBytes);
    toFreeList.pushManagedBufferSection(scratchSection);
    ID3D12Resource* const scratch = scratchBuffer.getBuffer();

    cmdList->SetPipelineState(pso.Get());
    cmdList->SetComputeRootSignature(rootSig.Get());

    const AreaLightCompactConstants constants = {
        .numRanges = static_cast<uint32_t>(ranges.size()),
        .numElements = numElements,
    };
    cmdList->SetComputeRoot32BitConstants(AREA_LIGHT_COMPACT_PARAM_IDX(CONSTANTS),
                                          sizeof(AreaLightCompactConstants) / 4, &constants, 0);
    cmdList->SetComputeRootShaderResourceView(AREA_LIGHT_COMPACT_PARAM_IDX(RANGES), rangesSection.getGpuVirtualAddress());
    const size_t firstByte = static_cast<size_t>(firstElement) * sizeof(uint32_t);
    cmdList->SetComputeRootShaderResourceView(AREA_LIGHT_COMPACT_PARAM_IDX(SRC), samplingStructure->GetGPUVirtualAddress() + firstByte);
    cmdList->SetComputeRootUnorderedAccessView(AREA_LIGHT_COMPACT_PARAM_IDX(DST), scratchSection.getGpuVirtualAddress());

    const Util::DispatchSize2D dispatchSize = Util::calculateDispatchSize2D(numElements, AREA_LIGHT_COMPACT_WORKGROUP_SIZE);
    cmdList->Dispatch(dispatchSize.x, dispatchSize.y, 1);

    BufferHelper::uavBarrier(cmdList, scratch);
    BufferHelper::copyBufferRegion(cmdList,
                                   samplingStructure,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   firstByte,
                                   scratch,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   scratchSection.offsetBytes,
                                   sizeBytes);
}

void destroy()
{
    rangesUploadBuffer.reset();
    scratchBuffer.reset();
    pso.Reset();
    rootSig.Reset();
}

} // namespace AreaLightCompactor
