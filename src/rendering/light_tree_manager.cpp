// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "light_tree_manager.h"

#include "buffer/buffer_helper.h"
#include "buffer/to_free_list.h"
#include "common/common_registers.h"
#include "common/common_settings.h"
#include "common/common_structs.h"
#include "gpu_sort/gpu_radix_sort.h"
#include "renderer/pipeline_builder.h"
#include "renderer/renderer_internal.h"
#include "renderer/shaders.h"
#include "util/util.h"

namespace Renderer
{

namespace
{

enum class EmitterCollectParam
{
    GLOBAL_PARAMS,

    INSTANCE_DATAS,
    MATERIALS,
    AREA_LIGHTS,
    AREA_LIGHT_SAMPLING_STRUCTURE,

    LIGHT_AUX_OUT,
    LIGHT_TO_LEAF_OUT,

    COUNT
};

enum class BufferClearParam
{
    CAPACITY_CONSTANT,

    LIGHT_AUX_OUT,
    LIGHT_TO_LEAF_OUT,

    COUNT
};

enum class SceneBboxResetParam
{
    SCENE_BBOX_OUT,

    COUNT
};

enum class BboxReduceParam
{
    CAPACITY_CONSTANT,

    LIGHT_AUX_OUT,
    SCENE_BBOX_OUT,

    COUNT
};

enum class MortonEmitParam
{
    GLOBAL_PARAMS,

    AREA_LIGHT_SAMPLING_STRUCTURE,

    LIGHT_AUX_OUT,
    SCENE_BBOX_OUT,
    MORTON_KEYS_OUT,
    MORTON_VALUES_OUT,

    COUNT
};

enum class LeafPopulateParam
{
    LEAF_POPULATE_CONSTANTS, // (numAreaLights, treeLeafBase)

    LIGHT_AUX_OUT,
    LIGHT_TO_LEAF_OUT,
    LIGHT_TREE_OUT,
    MORTON_VALUES_OUT,

    COUNT
};

enum class InternalLevelsParam
{
    INTERNAL_LEVELS_CONSTANTS, // (levelOffset, levelCount, depth)

    LIGHT_TREE_OUT,

    COUNT
};

#define EMITTER_COLLECT_PARAM_IDX(name) static_cast<uint32_t>(EmitterCollectParam::name)
#define BUFFER_CLEAR_PARAM_IDX(name) static_cast<uint32_t>(BufferClearParam::name)
#define SCENE_BBOX_RESET_PARAM_IDX(name) static_cast<uint32_t>(SceneBboxResetParam::name)
#define BBOX_REDUCE_PARAM_IDX(name) static_cast<uint32_t>(BboxReduceParam::name)
#define MORTON_EMIT_PARAM_IDX(name) static_cast<uint32_t>(MortonEmitParam::name)
#define LEAF_POPULATE_PARAM_IDX(name) static_cast<uint32_t>(LeafPopulateParam::name)
#define INTERNAL_LEVELS_PARAM_IDX(name) static_cast<uint32_t>(InternalLevelsParam::name)

// The sparse area-light count grows in coarse pow2 steps from this floor to
// avoid reallocating on every tiny topology change.
constexpr uint32_t LIGHT_AUX_CAPACITY_FLOOR = 256;

// All Stage 2 root constants land in the LIGHT_TREE b0 slot, with the cbuffer
// re-declared per shader to match the actual constant layout.
D3D12_ROOT_PARAMETER1 makeLightTreeRootConstants(uint32_t num32BitValues)
{
    return {
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
        .Constants = {
            .ShaderRegister = LIGHT_TREE_REGISTER_CONSTANTS,
            .RegisterSpace = LIGHT_TREE_REGISTER_SPACE,
            .Num32BitValues = num32BitValues,
        },
    };
}

// log2 of a pow2 value. Caller guarantees v is a power of two and v >= 1.
uint32_t log2OfPow2(uint32_t v)
{
    uint32_t r = 0;
    while ((1u << r) < v)
    {
        ++r;
    }
    return r;
}

} // namespace

void LightTreeManager::init()
{
    // -------------------------------------------------
    // Stage 1 — emitter collect
    // -------------------------------------------------
    {
        std::array<D3D12_ROOT_PARAMETER1, EMITTER_COLLECT_PARAM_IDX(COUNT)> params;
        params[EMITTER_COLLECT_PARAM_IDX(GLOBAL_PARAMS)] = MAKE_PARAM(CBV, COMMON, GLOBAL_PARAMS);
        params[EMITTER_COLLECT_PARAM_IDX(INSTANCE_DATAS)] = MAKE_PARAM(SRV, RT, INSTANCE_DATAS);
        params[EMITTER_COLLECT_PARAM_IDX(MATERIALS)] = MAKE_PARAM(SRV, RT, MATERIALS);
        params[EMITTER_COLLECT_PARAM_IDX(AREA_LIGHTS)] = MAKE_PARAM(SRV, RT, AREA_LIGHTS);
        params[EMITTER_COLLECT_PARAM_IDX(AREA_LIGHT_SAMPLING_STRUCTURE)] = MAKE_PARAM(SRV, RT, AREA_LIGHT_SAMPLING_STRUCTURE);
        params[EMITTER_COLLECT_PARAM_IDX(LIGHT_AUX_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, LIGHT_AUX_OUT);
        params[EMITTER_COLLECT_PARAM_IDX(LIGHT_TO_LEAF_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, LIGHT_TO_LEAF_OUT);

        serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                        nullptr, 0, this->emitterCollectRootSig);
        this->emitterCollectRootSig->SetName(L"emitterCollectRootSig");

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = this->emitterCollectRootSig.Get();
        psoDesc.CS = makeShaderBytecode(getShader("emitter_collect_cs"));
        CHECK_HRESULT(renderState.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&this->emitterCollectPso)));
        this->emitterCollectPso->SetName(L"emitterCollectPso");
    }

    // -------------------------------------------------
    // Stage 1 — buffer clear
    // -------------------------------------------------
    {
        std::array<D3D12_ROOT_PARAMETER1, BUFFER_CLEAR_PARAM_IDX(COUNT)> params;
        params[BUFFER_CLEAR_PARAM_IDX(CAPACITY_CONSTANT)] = makeLightTreeRootConstants(1);
        params[BUFFER_CLEAR_PARAM_IDX(LIGHT_AUX_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, LIGHT_AUX_OUT);
        params[BUFFER_CLEAR_PARAM_IDX(LIGHT_TO_LEAF_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, LIGHT_TO_LEAF_OUT);

        serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                        nullptr, 0, this->bufferClearRootSig);
        this->bufferClearRootSig->SetName(L"lightBufferClearRootSig");

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = this->bufferClearRootSig.Get();
        psoDesc.CS = makeShaderBytecode(getShader("light_buffer_clear_cs"));
        CHECK_HRESULT(renderState.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&this->bufferClearPso)));
        this->bufferClearPso->SetName(L"lightBufferClearPso");
    }

    // -------------------------------------------------
    // Stage 2 — scene bbox reset (1 workgroup, 6 threads)
    // -------------------------------------------------
    {
        std::array<D3D12_ROOT_PARAMETER1, SCENE_BBOX_RESET_PARAM_IDX(COUNT)> params;
        params[SCENE_BBOX_RESET_PARAM_IDX(SCENE_BBOX_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, SCENE_BBOX_OUT);

        serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                        nullptr, 0, this->sceneBboxResetRootSig);
        this->sceneBboxResetRootSig->SetName(L"lightTreeSceneBboxResetRootSig");

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = this->sceneBboxResetRootSig.Get();
        psoDesc.CS = makeShaderBytecode(getShader("light_tree_scene_bbox_reset_cs"));
        CHECK_HRESULT(renderState.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&this->sceneBboxResetPso)));
        this->sceneBboxResetPso->SetName(L"lightTreeSceneBboxResetPso");
    }

    // -------------------------------------------------
    // Stage 2 — bbox reduce
    // -------------------------------------------------
    {
        std::array<D3D12_ROOT_PARAMETER1, BBOX_REDUCE_PARAM_IDX(COUNT)> params;
        params[BBOX_REDUCE_PARAM_IDX(CAPACITY_CONSTANT)] = makeLightTreeRootConstants(1);
        params[BBOX_REDUCE_PARAM_IDX(LIGHT_AUX_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, LIGHT_AUX_OUT);
        params[BBOX_REDUCE_PARAM_IDX(SCENE_BBOX_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, SCENE_BBOX_OUT);

        serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                        nullptr, 0, this->bboxReduceRootSig);
        this->bboxReduceRootSig->SetName(L"lightTreeBboxReduceRootSig");

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = this->bboxReduceRootSig.Get();
        psoDesc.CS = makeShaderBytecode(getShader("light_tree_bbox_reduce_cs"));
        CHECK_HRESULT(renderState.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&this->bboxReducePso)));
        this->bboxReducePso->SetName(L"lightTreeBboxReducePso");
    }

    // -------------------------------------------------
    // Stage 2 — morton emit
    // -------------------------------------------------
    {
        std::array<D3D12_ROOT_PARAMETER1, MORTON_EMIT_PARAM_IDX(COUNT)> params;
        params[MORTON_EMIT_PARAM_IDX(GLOBAL_PARAMS)] = MAKE_PARAM(CBV, COMMON, GLOBAL_PARAMS);
        params[MORTON_EMIT_PARAM_IDX(AREA_LIGHT_SAMPLING_STRUCTURE)] = MAKE_PARAM(SRV, RT, AREA_LIGHT_SAMPLING_STRUCTURE);
        params[MORTON_EMIT_PARAM_IDX(LIGHT_AUX_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, LIGHT_AUX_OUT);
        params[MORTON_EMIT_PARAM_IDX(SCENE_BBOX_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, SCENE_BBOX_OUT);
        params[MORTON_EMIT_PARAM_IDX(MORTON_KEYS_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, MORTON_KEYS_OUT);
        params[MORTON_EMIT_PARAM_IDX(MORTON_VALUES_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, MORTON_VALUES_OUT);

        serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                        nullptr, 0, this->mortonEmitRootSig);
        this->mortonEmitRootSig->SetName(L"lightTreeMortonEmitRootSig");

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = this->mortonEmitRootSig.Get();
        psoDesc.CS = makeShaderBytecode(getShader("light_tree_morton_emit_cs"));
        CHECK_HRESULT(renderState.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&this->mortonEmitPso)));
        this->mortonEmitPso->SetName(L"lightTreeMortonEmitPso");
    }

    // -------------------------------------------------
    // Stage 2 — leaf populate + reverse scatter (fused)
    // -------------------------------------------------
    {
        std::array<D3D12_ROOT_PARAMETER1, LEAF_POPULATE_PARAM_IDX(COUNT)> params;
        params[LEAF_POPULATE_PARAM_IDX(LEAF_POPULATE_CONSTANTS)] = makeLightTreeRootConstants(2);
        params[LEAF_POPULATE_PARAM_IDX(LIGHT_AUX_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, LIGHT_AUX_OUT);
        params[LEAF_POPULATE_PARAM_IDX(LIGHT_TO_LEAF_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, LIGHT_TO_LEAF_OUT);
        params[LEAF_POPULATE_PARAM_IDX(LIGHT_TREE_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, LIGHT_TREE_OUT);
        params[LEAF_POPULATE_PARAM_IDX(MORTON_VALUES_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, MORTON_VALUES_OUT);

        serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                        nullptr, 0, this->leafPopulateRootSig);
        this->leafPopulateRootSig->SetName(L"lightTreeLeafPopulateRootSig");

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = this->leafPopulateRootSig.Get();
        psoDesc.CS = makeShaderBytecode(getShader("light_tree_leaf_populate_cs"));
        CHECK_HRESULT(renderState.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&this->leafPopulatePso)));
        this->leafPopulatePso->SetName(L"lightTreeLeafPopulatePso");
    }

    // -------------------------------------------------
    // Stage 2 — internal levels (bottom-up gather)
    // -------------------------------------------------
    {
        std::array<D3D12_ROOT_PARAMETER1, INTERNAL_LEVELS_PARAM_IDX(COUNT)> params;
        params[INTERNAL_LEVELS_PARAM_IDX(INTERNAL_LEVELS_CONSTANTS)] = makeLightTreeRootConstants(3);
        params[INTERNAL_LEVELS_PARAM_IDX(LIGHT_TREE_OUT)] = MAKE_PARAM(UAV, LIGHT_TREE, LIGHT_TREE_OUT);

        serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                        nullptr, 0, this->internalLevelsRootSig);
        this->internalLevelsRootSig->SetName(L"lightTreeInternalLevelsRootSig");

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = this->internalLevelsRootSig.Get();
        psoDesc.CS = makeShaderBytecode(getShader("light_tree_internal_levels_cs"));
        CHECK_HRESULT(renderState.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&this->internalLevelsPso)));
        this->internalLevelsPso->SetName(L"lightTreeInternalLevelsPso");
    }

    // dev_sceneBbox is fixed-size (6 floats = 24 B) and renderer-lifetime.
    // Allocate once; never resize, never reset across scene swaps.
    this->dev_sceneBbox = BufferHelper::createBasicBuffer(
        24ull,
        &DEFAULT_HEAP,
        { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    this->dev_sceneBbox->SetName(L"dev_sceneBbox");
}

void LightTreeManager::reset()
{
    this->dev_lightAux.Reset();
    this->dev_lightToLeaf.Reset();
    this->capacity = 0;

    this->dev_lightTree.Reset();
    this->dev_mortonKeys.Reset();
    this->dev_mortonValues.Reset();
    this->mortonCapacity = 0;
    this->treeNodeCapacity = 0;
}

void LightTreeManager::destroy()
{
    this->reset();
    this->dev_sceneBbox.Reset();

    this->emitterCollectPso.Reset();
    this->emitterCollectRootSig.Reset();
    this->bufferClearPso.Reset();
    this->bufferClearRootSig.Reset();

    this->sceneBboxResetPso.Reset();
    this->sceneBboxResetRootSig.Reset();
    this->bboxReducePso.Reset();
    this->bboxReduceRootSig.Reset();
    this->mortonEmitPso.Reset();
    this->mortonEmitRootSig.Reset();
    this->leafPopulatePso.Reset();
    this->leafPopulateRootSig.Reset();
    this->internalLevelsPso.Reset();
    this->internalLevelsRootSig.Reset();
}

void LightTreeManager::ensureCapacity(ToFreeList& toFreeList, uint32_t sparseCount)
{
    if (sparseCount <= this->capacity && this->dev_lightAux != nullptr)
    {
        return;
    }

    const uint32_t newCapacity = Util::nextPow2AtLeast(LIGHT_AUX_CAPACITY_FLOOR, sparseCount);

    if (this->dev_lightAux != nullptr)
    {
        toFreeList.pushResource(this->dev_lightAux, false /*isMapped*/);
        this->dev_lightAux.Reset();
    }
    if (this->dev_lightToLeaf != nullptr)
    {
        toFreeList.pushResource(this->dev_lightToLeaf, false /*isMapped*/);
        this->dev_lightToLeaf.Reset();
    }

    this->dev_lightAux = BufferHelper::createBasicBuffer(
        static_cast<uint64_t>(newCapacity) * sizeof(LightAux),
        &DEFAULT_HEAP,
        { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    this->dev_lightAux->SetName(L"dev_lightAux");

    this->dev_lightToLeaf = BufferHelper::createBasicBuffer(
        static_cast<uint64_t>(newCapacity) * sizeof(uint32_t),
        &DEFAULT_HEAP,
        { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    this->dev_lightToLeaf->SetName(L"dev_lightToLeaf");

    this->capacity = newCapacity;
}

void LightTreeManager::ensureLightTreeCapacity(ToFreeList& toFreeList, uint32_t numAreaLights)
{
    const uint32_t newM = Util::nextPow2AtLeast(LIGHT_TREE_LEAF_FLOOR, numAreaLights);
    if (newM <= this->mortonCapacity && this->dev_lightTree != nullptr)
    {
        return;
    }

    if (this->dev_lightTree != nullptr)
    {
        toFreeList.pushResource(this->dev_lightTree, false /*isMapped*/);
        this->dev_lightTree.Reset();
    }
    if (this->dev_mortonKeys != nullptr)
    {
        toFreeList.pushResource(this->dev_mortonKeys, false /*isMapped*/);
        this->dev_mortonKeys.Reset();
    }
    if (this->dev_mortonValues != nullptr)
    {
        toFreeList.pushResource(this->dev_mortonValues, false /*isMapped*/);
        this->dev_mortonValues.Reset();
    }

    const uint32_t newNodeCount = 2u * newM - 1u;

    this->dev_lightTree = BufferHelper::createBasicBuffer(
        static_cast<uint64_t>(newNodeCount) * sizeof(LightTreeNode),
        &DEFAULT_HEAP,
        { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    this->dev_lightTree->SetName(L"dev_lightTree");

    this->dev_mortonKeys = BufferHelper::createBasicBuffer(
        static_cast<uint64_t>(newM) * sizeof(uint32_t),
        &DEFAULT_HEAP,
        { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    this->dev_mortonKeys->SetName(L"dev_mortonKeys");

    this->dev_mortonValues = BufferHelper::createBasicBuffer(
        static_cast<uint64_t>(newM) * sizeof(uint32_t),
        &DEFAULT_HEAP,
        { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    this->dev_mortonValues->SetName(L"dev_mortonValues");

    this->mortonCapacity = newM;
    this->treeNodeCapacity = newNodeCount;
}

bool LightTreeManager::update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    if (!renderState.scene.didAreaLightTopologyChange())
    {
        return false;
    }

    const uint32_t sparseCount = renderState.scene.getAreaLightSparseCount();
    if (sparseCount == 0)
    {
        return false;
    }

    this->ensureCapacity(toFreeList, sparseCount);

    ParamBlockManager& paramBlockManager = renderState.frameCtxs[renderState.frameCtxIdx].paramBlockManager;

    // -------------------------------------------------
    // Stage 1 — Pass A: clear the full capacity to sentinel/zero
    // -------------------------------------------------
    {
        cmdList->SetPipelineState(this->bufferClearPso.Get());
        cmdList->SetComputeRootSignature(this->bufferClearRootSig.Get());

        cmdList->SetComputeRoot32BitConstant(BUFFER_CLEAR_PARAM_IDX(CAPACITY_CONSTANT), this->capacity, 0);
        cmdList->SetComputeRootUnorderedAccessView(BUFFER_CLEAR_PARAM_IDX(LIGHT_AUX_OUT),
                                                   this->dev_lightAux->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(BUFFER_CLEAR_PARAM_IDX(LIGHT_TO_LEAF_OUT),
                                                   this->dev_lightToLeaf->GetGPUVirtualAddress());

        const uint32_t clearDispatchSize = Util::calculateDispatchSize(this->capacity, LIGHT_BUFFER_CLEAR_WORKGROUP_SIZE);
        cmdList->Dispatch(clearDispatchSize, 1, 1);

        BufferHelper::uavBarrier(cmdList, this->dev_lightAux.Get());
        BufferHelper::uavBarrier(cmdList, this->dev_lightToLeaf.Get());
    }

    // -------------------------------------------------
    // Stage 1 — Pass B: fill live sparse slots from the scene
    // -------------------------------------------------
    {
        cmdList->SetPipelineState(this->emitterCollectPso.Get());
        cmdList->SetComputeRootSignature(this->emitterCollectRootSig.Get());

        cmdList->SetComputeRootConstantBufferView(EMITTER_COLLECT_PARAM_IDX(GLOBAL_PARAMS),
                                                  paramBlockManager.getParamBufferGpuAddress());
        cmdList->SetComputeRootShaderResourceView(EMITTER_COLLECT_PARAM_IDX(INSTANCE_DATAS),
                                                  renderState.scene.getDevInstanceDatasAddress());
        cmdList->SetComputeRootShaderResourceView(EMITTER_COLLECT_PARAM_IDX(MATERIALS),
                                                  renderState.scene.getDevMaterialsAddress());
        cmdList->SetComputeRootShaderResourceView(EMITTER_COLLECT_PARAM_IDX(AREA_LIGHTS),
                                                  renderState.scene.getDevAreaLightsBufferAddress());
        cmdList->SetComputeRootShaderResourceView(EMITTER_COLLECT_PARAM_IDX(AREA_LIGHT_SAMPLING_STRUCTURE),
                                                  renderState.scene.getDevAreaLightSamplingStructureAddress());
        cmdList->SetComputeRootUnorderedAccessView(EMITTER_COLLECT_PARAM_IDX(LIGHT_AUX_OUT),
                                                   this->dev_lightAux->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(EMITTER_COLLECT_PARAM_IDX(LIGHT_TO_LEAF_OUT),
                                                   this->dev_lightToLeaf->GetGPUVirtualAddress());

        const uint32_t numAreaLights = renderState.scene.getNumAreaLights();
        const uint32_t dispatchSize = Util::calculateDispatchSize(numAreaLights, EMITTER_COLLECT_WORKGROUP_SIZE);
        cmdList->Dispatch(dispatchSize, 1, 1);

        BufferHelper::uavBarrier(cmdList, this->dev_lightAux.Get());
        BufferHelper::uavBarrier(cmdList, this->dev_lightToLeaf.Get());
    }

    const uint32_t numAreaLights = renderState.scene.getNumAreaLights();
    // sparseCount > 0 implies numAreaLights > 0 (both are derived from the same
    // loop in Scene::makeTlas), but assert defensively — GpuRadixSort asserts
    // on numKeys == 0.
    if (numAreaLights == 0)
    {
        return true;
    }

    this->ensureLightTreeCapacity(toFreeList, numAreaLights);
    const uint32_t M = this->mortonCapacity;
    const uint32_t treeLeafBase = M - 1u;

    // -------------------------------------------------
    // Stage 2 — Pass 0: reset dev_sceneBbox to ±inf sentinels
    // -------------------------------------------------
    {
        cmdList->SetPipelineState(this->sceneBboxResetPso.Get());
        cmdList->SetComputeRootSignature(this->sceneBboxResetRootSig.Get());

        cmdList->SetComputeRootUnorderedAccessView(SCENE_BBOX_RESET_PARAM_IDX(SCENE_BBOX_OUT),
                                                   this->dev_sceneBbox->GetGPUVirtualAddress());
        cmdList->Dispatch(1, 1, 1);

        BufferHelper::uavBarrier(cmdList, this->dev_sceneBbox.Get());
    }

    // -------------------------------------------------
    // Stage 2 — Pass 1: bbox reduce over dev_lightAux into dev_sceneBbox
    // -------------------------------------------------
    {
        cmdList->SetPipelineState(this->bboxReducePso.Get());
        cmdList->SetComputeRootSignature(this->bboxReduceRootSig.Get());

        cmdList->SetComputeRoot32BitConstant(BBOX_REDUCE_PARAM_IDX(CAPACITY_CONSTANT), this->capacity, 0);
        cmdList->SetComputeRootUnorderedAccessView(BBOX_REDUCE_PARAM_IDX(LIGHT_AUX_OUT),
                                                   this->dev_lightAux->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(BBOX_REDUCE_PARAM_IDX(SCENE_BBOX_OUT),
                                                   this->dev_sceneBbox->GetGPUVirtualAddress());

        const uint32_t bboxReduceDispatchSize =
            Util::calculateDispatchSize(this->capacity, LIGHT_TREE_BBOX_REDUCE_WORKGROUP_SIZE);
        cmdList->Dispatch(bboxReduceDispatchSize, 1, 1);

        BufferHelper::uavBarrier(cmdList, this->dev_sceneBbox.Get());
    }

    // -------------------------------------------------
    // Stage 2 — Pass 2: morton emit
    // -------------------------------------------------
    {
        cmdList->SetPipelineState(this->mortonEmitPso.Get());
        cmdList->SetComputeRootSignature(this->mortonEmitRootSig.Get());

        cmdList->SetComputeRootConstantBufferView(MORTON_EMIT_PARAM_IDX(GLOBAL_PARAMS),
                                                  paramBlockManager.getParamBufferGpuAddress());
        cmdList->SetComputeRootShaderResourceView(MORTON_EMIT_PARAM_IDX(AREA_LIGHT_SAMPLING_STRUCTURE),
                                                  renderState.scene.getDevAreaLightSamplingStructureAddress());
        cmdList->SetComputeRootUnorderedAccessView(MORTON_EMIT_PARAM_IDX(LIGHT_AUX_OUT),
                                                   this->dev_lightAux->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(MORTON_EMIT_PARAM_IDX(SCENE_BBOX_OUT),
                                                   this->dev_sceneBbox->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(MORTON_EMIT_PARAM_IDX(MORTON_KEYS_OUT),
                                                   this->dev_mortonKeys->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(MORTON_EMIT_PARAM_IDX(MORTON_VALUES_OUT),
                                                   this->dev_mortonValues->GetGPUVirtualAddress());

        const uint32_t mortonEmitDispatchSize =
            Util::calculateDispatchSize(numAreaLights, LIGHT_TREE_MORTON_EMIT_WORKGROUP_SIZE);
        cmdList->Dispatch(mortonEmitDispatchSize, 1, 1);

        BufferHelper::uavBarrier(cmdList, this->dev_mortonKeys.Get());
        BufferHelper::uavBarrier(cmdList, this->dev_mortonValues.Get());
    }

    // -------------------------------------------------
    // Stage 2 — Pass 3: radix sort (mortonKey, sparseIdx) pairs in-place
    // -------------------------------------------------
    renderState.gpuRadixSort.dispatch(cmdList,
                                      toFreeList,
                                      this->dev_mortonKeys.Get(),
                                      this->dev_mortonValues.Get(),
                                      numAreaLights);
    // GpuRadixSort barriers both buffers internally; both end in UAV state.

    // -------------------------------------------------
    // Stage 2 — Pass 4: leaf populate + reverse scatter (fused)
    // -------------------------------------------------
    {
        cmdList->SetPipelineState(this->leafPopulatePso.Get());
        cmdList->SetComputeRootSignature(this->leafPopulateRootSig.Get());

        const uint32_t leafPopulateConsts[2] = { numAreaLights, treeLeafBase };
        cmdList->SetComputeRoot32BitConstants(LEAF_POPULATE_PARAM_IDX(LEAF_POPULATE_CONSTANTS),
                                              2, leafPopulateConsts, 0);
        cmdList->SetComputeRootUnorderedAccessView(LEAF_POPULATE_PARAM_IDX(LIGHT_AUX_OUT),
                                                   this->dev_lightAux->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(LEAF_POPULATE_PARAM_IDX(LIGHT_TO_LEAF_OUT),
                                                   this->dev_lightToLeaf->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(LEAF_POPULATE_PARAM_IDX(LIGHT_TREE_OUT),
                                                   this->dev_lightTree->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(LEAF_POPULATE_PARAM_IDX(MORTON_VALUES_OUT),
                                                   this->dev_mortonValues->GetGPUVirtualAddress());

        const uint32_t leafDispatchSize = Util::calculateDispatchSize(M, LIGHT_TREE_LEAF_POPULATE_WORKGROUP_SIZE);
        cmdList->Dispatch(leafDispatchSize, 1, 1);

        BufferHelper::uavBarrier(cmdList, this->dev_lightTree.Get());
        BufferHelper::uavBarrier(cmdList, this->dev_lightToLeaf.Get());
    }

    // -------------------------------------------------
    // Stage 2 — Pass 5: internal levels (bottom-up, depth=2 per dispatch,
    // last may be depth=1 when log2(M) is odd)
    // -------------------------------------------------
    {
        cmdList->SetPipelineState(this->internalLevelsPso.Get());
        cmdList->SetComputeRootSignature(this->internalLevelsRootSig.Get());
        cmdList->SetComputeRootUnorderedAccessView(INTERNAL_LEVELS_PARAM_IDX(LIGHT_TREE_OUT),
                                                   this->dev_lightTree->GetGPUVirtualAddress());

        uint32_t levelsRemaining = log2OfPow2(M);
        while (levelsRemaining > 0u)
        {
            const uint32_t depth = (levelsRemaining >= 2u) ? 2u : 1u;
            const uint32_t topLevel = levelsRemaining - depth;
            const uint32_t levelOffset = (1u << topLevel) - 1u;
            const uint32_t levelCount = (1u << topLevel);

            const uint32_t internalConsts[3] = { levelOffset, levelCount, depth };
            cmdList->SetComputeRoot32BitConstants(INTERNAL_LEVELS_PARAM_IDX(INTERNAL_LEVELS_CONSTANTS),
                                                  3, internalConsts, 0);

            const uint32_t dispatchSize =
                Util::calculateDispatchSize(levelCount, LIGHT_TREE_INTERNAL_LEVELS_WORKGROUP_SIZE);
            cmdList->Dispatch(dispatchSize, 1, 1);

            BufferHelper::uavBarrier(cmdList, this->dev_lightTree.Get());

            levelsRemaining -= depth;
        }
    }

    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS LightTreeManager::getDevLightAuxAddress() const
{
    return this->dev_lightAux ? this->dev_lightAux->GetGPUVirtualAddress() : 0;
}

D3D12_GPU_VIRTUAL_ADDRESS LightTreeManager::getDevLightToLeafAddress() const
{
    return this->dev_lightToLeaf ? this->dev_lightToLeaf->GetGPUVirtualAddress() : 0;
}

D3D12_GPU_VIRTUAL_ADDRESS LightTreeManager::getDevLightTreeAddress() const
{
    return this->dev_lightTree ? this->dev_lightTree->GetGPUVirtualAddress() : 0;
}

D3D12_GPU_VIRTUAL_ADDRESS LightTreeManager::getDevSceneBboxAddress() const
{
    return this->dev_sceneBbox ? this->dev_sceneBbox->GetGPUVirtualAddress() : 0;
}

uint32_t LightTreeManager::getCurrentTreeLeafCount() const
{
    return this->mortonCapacity;
}

uint32_t LightTreeManager::getCurrentTreeNodeCount() const
{
    return this->treeNodeCapacity;
}

} // namespace Renderer
