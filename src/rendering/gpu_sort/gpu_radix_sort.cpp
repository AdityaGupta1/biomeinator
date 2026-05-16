// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "gpu_radix_sort.h"

#include "debug.h"
#include "rendering/buffer/buffer_helper.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/renderer/pipeline_builder.h"
#include "rendering/renderer/renderer_internal.h"
#include "rendering/renderer/shaders.h"
#include "util/util.h"

#include <array>

namespace Renderer
{

namespace
{

// GPUSorting register slots are dictated by external/GPUSorting HLSL
// (SortCommon.hlsl). Use raw register numbers — they cannot be remapped
// without forking upstream. The CB and SORT slots both use the literal
// number 0; that is not a collision because b0 (CBV) and u0 (UAV) live in
// distinct register namespaces.
constexpr uint32_t GPU_SORT_REG_SPACE = 0;
constexpr uint32_t GPU_SORT_REG_CB = 0;          // cbGpuSorting : register(b0)
constexpr uint32_t GPU_SORT_REG_SORT = 0;        // b_sort        : register(u0)
constexpr uint32_t GPU_SORT_REG_ALT = 1;         // b_alt         : register(u1)
constexpr uint32_t GPU_SORT_REG_SORT_PAYLOAD = 2;// b_sortPayload : register(u2)
constexpr uint32_t GPU_SORT_REG_ALT_PAYLOAD = 3; // b_altPayload  : register(u3)
constexpr uint32_t GPU_SORT_REG_GLOBAL_HIST = 4; // b_globalHist  : register(u4)
constexpr uint32_t GPU_SORT_REG_PASS_HIST = 5;   // b_passHist    : register(u5)

constexpr uint32_t SCRATCH_CAPACITY_FLOOR = 1024;

void replaceScratchUavBuffer(ToFreeList& toFreeList,
                             ComPtr<ID3D12Resource>& buf,
                             uint64_t bytes,
                             const wchar_t* debugName)
{
    if (buf != nullptr)
    {
        toFreeList.pushResource(buf, false /*isMapped*/);
    }
    buf = BufferHelper::createBasicBuffer(
        bytes,
        &DEFAULT_HEAP,
        { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    buf->SetName(debugName);
}

D3D12_ROOT_PARAMETER1 makeRootUav(uint32_t reg)
{
    return {
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV,
        .Descriptor = { .ShaderRegister = reg, .RegisterSpace = GPU_SORT_REG_SPACE },
    };
}

D3D12_ROOT_PARAMETER1 makeRootConstants(uint32_t reg, uint32_t num32BitValues)
{
    return {
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
        .Constants = {
            .ShaderRegister = reg,
            .RegisterSpace = GPU_SORT_REG_SPACE,
            .Num32BitValues = num32BitValues,
        },
    };
}

void createComputePso(ID3D12RootSignature* rootSig,
                      std::string_view shaderName,
                      ComPtr<ID3D12PipelineState>& outPso,
                      const wchar_t* debugName)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig;
    psoDesc.CS = makeShaderBytecode(getShader(shaderName));
    CHECK_HRESULT(renderState.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPso)));
    outPso->SetName(debugName);
}

} // namespace

void GpuRadixSort::init()
{
    // The in-place dispatch contract relies on an even pass count so the final
    // Downsweep lands back in the caller's keys/values. If RADIX_PASSES ever
    // changes (e.g. a 24-bit key path), the dispatcher needs a final copy.
    static_assert(RADIX_PASSES % 2 == 0,
                  "GpuRadixSort in-place ping-pong requires an even pass count");

    // Root signature layouts mirror DeviceRadixSortKernels.h exactly.
    // Slot 0 of each is a 4×u32 root-constants block (cbGpuSorting) except for
    // Init, which only needs the global histogram UAV.

    // --- Init ---
    {
        std::array<D3D12_ROOT_PARAMETER1, 1> params = {
            makeRootUav(GPU_SORT_REG_GLOBAL_HIST),
        };
        serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                        nullptr, 0, this->initRootSig);
        this->initRootSig->SetName(L"gpuRadixSortInitRootSig");
        createComputePso(this->initRootSig.Get(), "gpu_sort_init_cs", this->initPso, L"gpuRadixSortInitPso");
    }

    // --- Upsweep ---
    {
        std::array<D3D12_ROOT_PARAMETER1, 4> params = {
            makeRootConstants(GPU_SORT_REG_CB, 4),
            makeRootUav(GPU_SORT_REG_SORT),
            makeRootUav(GPU_SORT_REG_GLOBAL_HIST),
            makeRootUav(GPU_SORT_REG_PASS_HIST),
        };
        serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                        nullptr, 0, this->upsweepRootSig);
        this->upsweepRootSig->SetName(L"gpuRadixSortUpsweepRootSig");
        createComputePso(this->upsweepRootSig.Get(), "gpu_sort_upsweep_cs", this->upsweepPso, L"gpuRadixSortUpsweepPso");
    }

    // --- Scan ---
    {
        std::array<D3D12_ROOT_PARAMETER1, 2> params = {
            makeRootConstants(GPU_SORT_REG_CB, 4),
            makeRootUav(GPU_SORT_REG_PASS_HIST),
        };
        serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                        nullptr, 0, this->scanRootSig);
        this->scanRootSig->SetName(L"gpuRadixSortScanRootSig");
        createComputePso(this->scanRootSig.Get(), "gpu_sort_scan_cs", this->scanPso, L"gpuRadixSortScanPso");
    }

    // --- Downsweep ---
    {
        std::array<D3D12_ROOT_PARAMETER1, 7> params = {
            makeRootConstants(GPU_SORT_REG_CB, 4),
            makeRootUav(GPU_SORT_REG_SORT),
            makeRootUav(GPU_SORT_REG_SORT_PAYLOAD),
            makeRootUav(GPU_SORT_REG_ALT),
            makeRootUav(GPU_SORT_REG_ALT_PAYLOAD),
            makeRootUav(GPU_SORT_REG_GLOBAL_HIST),
            makeRootUav(GPU_SORT_REG_PASS_HIST),
        };
        serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                        nullptr, 0, this->downsweepRootSig);
        this->downsweepRootSig->SetName(L"gpuRadixSortDownsweepRootSig");
        createComputePso(this->downsweepRootSig.Get(), "gpu_sort_downsweep_cs", this->downsweepPso,
                         L"gpuRadixSortDownsweepPso");
    }

    // Global histogram is fixed-size (RADIX * RADIX_PASSES = 1024 uint32s),
    // independent of input. Allocate once.
    this->dev_globalHist = BufferHelper::createBasicBuffer(
        static_cast<uint64_t>(RADIX) * RADIX_PASSES * sizeof(uint32_t),
        &DEFAULT_HEAP,
        { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    this->dev_globalHist->SetName(L"gpuRadixSort_globalHist");
}

void GpuRadixSort::destroy()
{
    this->dev_altKeys.Reset();
    this->dev_altValues.Reset();
    this->dev_passHist.Reset();
    this->dev_globalHist.Reset();
    this->scratchCapacity = 0;
    this->passHistThreadBlocks = 0;

    this->initPso.Reset();
    this->initRootSig.Reset();
    this->upsweepPso.Reset();
    this->upsweepRootSig.Reset();
    this->scanPso.Reset();
    this->scanRootSig.Reset();
    this->downsweepPso.Reset();
    this->downsweepRootSig.Reset();
}

void GpuRadixSort::ensureScratchCapacity(ToFreeList& toFreeList, uint32_t numKeys)
{
    const uint32_t needThreadBlocks = (numKeys + PARTITION_SIZE - 1) / PARTITION_SIZE;
    const bool capExceeded = numKeys > this->scratchCapacity || this->dev_altKeys == nullptr;
    const bool passHistExceeded = needThreadBlocks > this->passHistThreadBlocks || this->dev_passHist == nullptr;
    if (!capExceeded && !passHistExceeded)
    {
        return;
    }

    if (capExceeded)
    {
        const uint32_t newCapacity = Util::nextPow2AtLeast(SCRATCH_CAPACITY_FLOOR, numKeys);
        const uint64_t bytes = static_cast<uint64_t>(newCapacity) * sizeof(uint32_t);

        replaceScratchUavBuffer(toFreeList, this->dev_altKeys, bytes, L"gpuRadixSort_altKeys");
        replaceScratchUavBuffer(toFreeList, this->dev_altValues, bytes, L"gpuRadixSort_altValues");

        this->scratchCapacity = newCapacity;
    }

    if (passHistExceeded)
    {
        // passHist is sized by thread-block count, not raw key count. Grow it
        // in pow2 steps independently so it tracks the partition-rounded need.
        const uint32_t newThreadBlocks = Util::nextPow2AtLeast(1, needThreadBlocks);
        const uint64_t bytes = static_cast<uint64_t>(RADIX) * newThreadBlocks * sizeof(uint32_t);

        replaceScratchUavBuffer(toFreeList, this->dev_passHist, bytes, L"gpuRadixSort_passHist");

        this->passHistThreadBlocks = newThreadBlocks;
    }
}

void GpuRadixSort::dispatch(ID3D12GraphicsCommandList* cmdList,
                            ToFreeList& toFreeList,
                            ID3D12Resource* keysBuffer,
                            ID3D12Resource* valuesBuffer,
                            uint32_t numKeys)
{
    ASSERT(numKeys > 0, "GpuRadixSort::dispatch numKeys must be > 0");
    ASSERT(numKeys <= MAX_KEYS, "GpuRadixSort::dispatch numKeys exceeds single-dispatch limit");

    this->ensureScratchCapacity(toFreeList, numKeys);

    const uint32_t threadBlocks = (numKeys + PARTITION_SIZE - 1) / PARTITION_SIZE;

    const D3D12_GPU_VIRTUAL_ADDRESS keysGva = keysBuffer->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS valsGva = valuesBuffer->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS altKeyGva = this->dev_altKeys->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS altValGva = this->dev_altValues->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS globalGva = this->dev_globalHist->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS passGva = this->dev_passHist->GetGPUVirtualAddress();

    // -------------------------------------------------
    // 1. Init: zero the global histogram once
    // -------------------------------------------------
    cmdList->SetPipelineState(this->initPso.Get());
    cmdList->SetComputeRootSignature(this->initRootSig.Get());
    cmdList->SetComputeRootUnorderedAccessView(0, globalGva);
    cmdList->Dispatch(1, 1, 1);
    BufferHelper::uavBarrier(cmdList, this->dev_globalHist.Get());

    // -------------------------------------------------
    // 2. Four LSD radix passes (8-bit digits)
    //    After pass 3, the caller's keys/values hold the final sorted result.
    // -------------------------------------------------
    for (uint32_t pass = 0; pass < RADIX_PASSES; ++pass)
    {
        const uint32_t radixShift = pass * 8;
        const bool sortIsCallers = (pass % 2 == 0);
        const D3D12_GPU_VIRTUAL_ADDRESS sortKey = sortIsCallers ? keysGva : altKeyGva;
        const D3D12_GPU_VIRTUAL_ADDRESS altKey = sortIsCallers ? altKeyGva : keysGva;
        const D3D12_GPU_VIRTUAL_ADDRESS sortVal = sortIsCallers ? valsGva : altValGva;
        const D3D12_GPU_VIRTUAL_ADDRESS altVal = sortIsCallers ? altValGva : valsGva;

        const uint32_t cb[4] = { numKeys, radixShift, threadBlocks, /*isPartial=*/0 };

        // --- Upsweep ---
        cmdList->SetPipelineState(this->upsweepPso.Get());
        cmdList->SetComputeRootSignature(this->upsweepRootSig.Get());
        cmdList->SetComputeRoot32BitConstants(0, 4, cb, 0);
        cmdList->SetComputeRootUnorderedAccessView(1, sortKey);
        cmdList->SetComputeRootUnorderedAccessView(2, globalGva);
        cmdList->SetComputeRootUnorderedAccessView(3, passGva);
        cmdList->Dispatch(threadBlocks, 1, 1);
        BufferHelper::uavBarrier(cmdList, this->dev_passHist.Get());
        BufferHelper::uavBarrier(cmdList, this->dev_globalHist.Get());

        // --- Scan ---
        cmdList->SetPipelineState(this->scanPso.Get());
        cmdList->SetComputeRootSignature(this->scanRootSig.Get());
        cmdList->SetComputeRoot32BitConstants(0, 4, cb, 0);
        cmdList->SetComputeRootUnorderedAccessView(1, passGva);
        cmdList->Dispatch(RADIX, 1, 1);
        BufferHelper::uavBarrier(cmdList, this->dev_passHist.Get());

        // --- Downsweep ---
        cmdList->SetPipelineState(this->downsweepPso.Get());
        cmdList->SetComputeRootSignature(this->downsweepRootSig.Get());
        cmdList->SetComputeRoot32BitConstants(0, 4, cb, 0);
        cmdList->SetComputeRootUnorderedAccessView(1, sortKey);
        cmdList->SetComputeRootUnorderedAccessView(2, sortVal);
        cmdList->SetComputeRootUnorderedAccessView(3, altKey);
        cmdList->SetComputeRootUnorderedAccessView(4, altVal);
        cmdList->SetComputeRootUnorderedAccessView(5, globalGva);
        cmdList->SetComputeRootUnorderedAccessView(6, passGva);
        cmdList->Dispatch(threadBlocks, 1, 1);

        // Downsweep wrote into the alt-side resources for this pass; the next
        // pass will read from there. Barrier both since the role flips.
        ID3D12Resource* writtenKeys = sortIsCallers ? this->dev_altKeys.Get() : keysBuffer;
        ID3D12Resource* writtenVals = sortIsCallers ? this->dev_altValues.Get() : valuesBuffer;
        BufferHelper::uavBarrier(cmdList, writtenKeys);
        BufferHelper::uavBarrier(cmdList, writtenVals);
    }
}

} // namespace Renderer
