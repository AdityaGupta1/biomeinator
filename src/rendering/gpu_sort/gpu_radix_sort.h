// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "rendering/dxr_includes.h"

class ToFreeList;

namespace Renderer
{

// Wraps GPUSorting's DeviceRadixSort (external/GPUSorting). Sorts 32-bit uint
// keys with 32-bit uint values, ascending, in-place. Owns scratch buffers
// (alt-keys, alt-values, globalHist, passHist) that grow on demand.
//
// Tuning is baked at shader-compile time to GPUSorting's NVIDIA pairs preset:
// KEYS_PER_THREAD=15, D_DIM=512, PART_SIZE=7680. See CMakeLists.txt.
class GpuRadixSort
{
public:
    void init();
    void destroy();

    // numKeys must be > 0 and <= MAX_KEYS. Caller-owned key/value buffers must
    // be in D3D12_RESOURCE_STATE_UNORDERED_ACCESS on entry; they remain in
    // that state on exit. After dispatch returns, both buffers hold the sorted
    // result in-place (4 radix passes ping-pong sort/alt and land back on
    // caller storage on the final pass).
    void dispatch(ID3D12GraphicsCommandList* cmdList,
                  ToFreeList& toFreeList,
                  ID3D12Resource* keysBuffer,
                  ID3D12Resource* valuesBuffer,
                  uint32_t numKeys);

    // Largest input supported without multi-dispatch. The GPUSorting shaders
    // support splitting >65535 thread blocks across two dispatches; we do not
    // wire that yet. With PART_SIZE=7680, the cap is 65535 * 7680 ≈ 503M keys.
    static constexpr uint32_t PARTITION_SIZE = 7680;
    static constexpr uint32_t RADIX = 256;
    static constexpr uint32_t RADIX_PASSES = 4;
    static constexpr uint32_t MAX_KEYS = 65535u * PARTITION_SIZE;

private:
    ComPtr<ID3D12RootSignature> initRootSig{ nullptr };
    ComPtr<ID3D12PipelineState> initPso{ nullptr };
    ComPtr<ID3D12RootSignature> upsweepRootSig{ nullptr };
    ComPtr<ID3D12PipelineState> upsweepPso{ nullptr };
    ComPtr<ID3D12RootSignature> scanRootSig{ nullptr };
    ComPtr<ID3D12PipelineState> scanPso{ nullptr };
    ComPtr<ID3D12RootSignature> downsweepRootSig{ nullptr };
    ComPtr<ID3D12PipelineState> downsweepPso{ nullptr };

    ComPtr<ID3D12Resource> dev_altKeys{ nullptr };
    ComPtr<ID3D12Resource> dev_altValues{ nullptr };
    ComPtr<ID3D12Resource> dev_globalHist{ nullptr };
    ComPtr<ID3D12Resource> dev_passHist{ nullptr };
    uint32_t scratchCapacity{ 0 };
    uint32_t passHistThreadBlocks{ 0 };

    void ensureScratchCapacity(ToFreeList& toFreeList, uint32_t numKeys);
};

} // namespace Renderer
