// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "dxr_includes.h"

class ToFreeList;

namespace Renderer
{

// Stage 1 of the Real-Time Stochastic Lightcuts plan (see plans/plan.md).
// Owns per-frame buffers parallel to the scene's `areaLights` buffer:
//   * dev_lightAux       — LightAux[sparseCapacity] (bbox + flux), keyed by the SPARSE areaLights[] index
//   * dev_lightToLeaf    — uint[sparseCapacity] keyed identically; Stage 2 will scatter leaf indices in
//
// Both buffers are cleared to zero / LEAF_IDX_INVALID on every topology change,
// then emitter_collect overwrites the slots that are live in the current sampling
// structure. Slots whose triangles are not reachable from the current TLAS are
// left at the sentinel — Stage 2 reads only live slots (via the sampling
// structure) and Stage 4's BSDF-hit recovery only queries live areaLightIdxs.
class LightTreeManager
{
public:
    void init();
    // Frees per-scene GPU resources. Safe to call repeatedly across scene swaps;
    // PSOs / root sigs stay alive until destroy().
    void reset();
    // Releases pipelines and per-scene resources. Mirrors the rest of the
    // renderer's explicit teardown of D3D12 objects before device release.
    void destroy();

    // Returns true if a dispatch was recorded.
    bool update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

    D3D12_GPU_VIRTUAL_ADDRESS getDevLightAuxAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevLightToLeafAddress() const;

private:
    ComPtr<ID3D12RootSignature> emitterCollectRootSig{ nullptr };
    ComPtr<ID3D12PipelineState> emitterCollectPso{ nullptr };

    ComPtr<ID3D12RootSignature> bufferClearRootSig{ nullptr };
    ComPtr<ID3D12PipelineState> bufferClearPso{ nullptr };

    ComPtr<ID3D12Resource> dev_lightAux{ nullptr };
    ComPtr<ID3D12Resource> dev_lightToLeaf{ nullptr };
    uint32_t capacity{ 0 };

    void ensureCapacity(ToFreeList& toFreeList, uint32_t sparseCount);
};

} // namespace Renderer
