// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "dxr_includes.h"

class ToFreeList;

namespace Renderer
{

// Stages 1+2 of the Real-Time Stochastic Lightcuts plan (see plans/plan.md).
//
// Stage 1 owns per-frame buffers parallel to the scene's `areaLights` buffer:
//   * dev_lightAux       — LightAux[sparseCapacity] (bbox + flux), keyed by the SPARSE areaLights[] index
//   * dev_lightToLeaf    — uint[sparseCapacity] mapping sparseIdx -> leafIdx in dev_lightTree
//
// Both buffers are cleared to sentinel on every topology change, then
// emitter_collect overwrites the slots that are live in the current sampling
// structure. Stage 2 scatters leaf indices into dev_lightToLeaf after the sort.
//
// Stage 2 builds the perfect-binary light tree from those Stage 1 outputs:
//   * dev_sceneBbox      — 6 floats (orderable-uint encoded) reduced from LightAux bboxes
//   * dev_mortonKeys     — uint32 morton code per live light, sorted ascending
//   * dev_mortonValues   — uint32 sparseIdx, sorted by morton (the radix-sort payload)
//   * dev_lightTree      — LightTreeNode[2M-1] perfect binary tree, M = nextPow2(numAreaLights),
//                          leaves at [M-1, 2M-1), root at [0]
class LightTreeManager
{
public:
    void init();
    // Releases pipelines and per-scene resources. Mirrors the rest of the
    // renderer's explicit teardown of D3D12 objects before device release.
    void destroy();

    // Returns true if a dispatch was recorded.
    bool update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

    // Transitions the SRV-bound buffers (dev_lightTree, dev_lightToLeaf) from
    // UNORDERED_ACCESS → NON_PIXEL_SHADER_RESOURCE, but only for buffers that
    // were actually written by the most recent update() call. Buffers that
    // weren't written this frame are still in COMMON (post-decay) and will
    // implicit-promote to SRV on first raygen read.
    //
    // Must be called once per frame between update() and the path tracing
    // dispatch.
    void transitionForPathTracingRead(ID3D12GraphicsCommandList4* cmdList);

    uint32_t getTreeLeafCapacity() const; // M = nextPow2(numAreaLights)

    // SRV-bind addresses for path tracing. Fall back to a permanent placeholder
    // buffer when the real buffer is not allocated (empty scene), so root-SRV
    // bindings never see GPUVA == 0 (which is a validation error).
    D3D12_GPU_VIRTUAL_ADDRESS getDevLightTreeSrvBindAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS getDevLightToLeafSrvBindAddress() const;

private:
    // Stage 1 PSOs
    ComPtr<ID3D12RootSignature> emitterCollectRootSig{ nullptr };
    ComPtr<ID3D12PipelineState> emitterCollectPso{ nullptr };

    ComPtr<ID3D12RootSignature> bufferClearRootSig{ nullptr };
    ComPtr<ID3D12PipelineState> bufferClearPso{ nullptr };

    // Stage 2 PSOs
    ComPtr<ID3D12RootSignature> sceneBboxResetRootSig{ nullptr };
    ComPtr<ID3D12PipelineState> sceneBboxResetPso{ nullptr };

    ComPtr<ID3D12RootSignature> bboxReduceRootSig{ nullptr };
    ComPtr<ID3D12PipelineState> bboxReducePso{ nullptr };

    ComPtr<ID3D12RootSignature> mortonEmitRootSig{ nullptr };
    ComPtr<ID3D12PipelineState> mortonEmitPso{ nullptr };

    ComPtr<ID3D12RootSignature> leafPopulateRootSig{ nullptr };
    ComPtr<ID3D12PipelineState> leafPopulatePso{ nullptr };

    ComPtr<ID3D12RootSignature> internalLevelsRootSig{ nullptr };
    ComPtr<ID3D12PipelineState> internalLevelsPso{ nullptr };

    // Stage 1 resources
    ComPtr<ID3D12Resource> dev_lightAux{ nullptr };
    ComPtr<ID3D12Resource> dev_lightToLeaf{ nullptr };
    uint32_t capacity{ 0 };

    // Stage 2 resources
    ComPtr<ID3D12Resource> dev_lightTree{ nullptr };
    ComPtr<ID3D12Resource> dev_sceneBbox{ nullptr }; // 24 B, allocated once in init(), never resized
    ComPtr<ID3D12Resource> dev_mortonKeys{ nullptr };
    ComPtr<ID3D12Resource> dev_mortonValues{ nullptr };
    uint32_t mortonCapacity{ 0 }; // == M = nextPow2(numAreaLights)

    // 32-byte read-only placeholder, allocated once in init(). Returned by the
    // *SrvBindAddress() getters when the real buffer is not yet allocated.
    // Shader reads are gated by rtslParams.treeLeafCount == 0, so contents
    // never matter.
    ComPtr<ID3D12Resource> dev_srvPlaceholder{ nullptr };

    // Set inside update() for each buffer actually written this frame.
    // Consumed by transitionForPathTracingRead(). Reset to false at the top
    // of every update() call.
    bool wroteLightTreeThisCall{ false };
    bool wroteLightToLeafThisCall{ false };

    void ensureCapacity(ToFreeList& toFreeList, uint32_t sparseCount);
    void ensureLightTreeCapacity(ToFreeList& toFreeList, uint32_t numAreaLights);
};

} // namespace Renderer
