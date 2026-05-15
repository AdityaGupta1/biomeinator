// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "light_tree_manager.h"

#include "buffer/buffer_helper.h"
#include "buffer/to_free_list.h"
#include "common/common_registers.h"
#include "common/common_settings.h"
#include "common/common_structs.h"
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

#define EMITTER_COLLECT_PARAM_IDX(name) static_cast<uint32_t>(EmitterCollectParam::name)
#define BUFFER_CLEAR_PARAM_IDX(name) static_cast<uint32_t>(BufferClearParam::name)

// Round-up helper kept local to this manager. The sparse area-light count grows
// in coarse pow2 steps to avoid reallocating on every tiny topology change.
uint32_t nextCapacity(uint32_t sparseCount)
{
    uint32_t cap = 256;
    while (cap < sparseCount)
    {
        cap *= 2;
    }
    return cap;
}

} // namespace

void LightTreeManager::init()
{
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

    {
        std::array<D3D12_ROOT_PARAMETER1, BUFFER_CLEAR_PARAM_IDX(COUNT)> params;
        params[BUFFER_CLEAR_PARAM_IDX(CAPACITY_CONSTANT)] = {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
            .Constants = {
                .ShaderRegister = LIGHT_TREE_REGISTER_CONSTANTS,
                .RegisterSpace = LIGHT_TREE_REGISTER_SPACE,
                .Num32BitValues = 1,
            },
        };
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
}

void LightTreeManager::reset()
{
    this->dev_lightAux.Reset();
    this->dev_lightToLeaf.Reset();
    this->capacity = 0;
}

void LightTreeManager::destroy()
{
    this->reset();
    this->emitterCollectPso.Reset();
    this->emitterCollectRootSig.Reset();
    this->bufferClearPso.Reset();
    this->bufferClearRootSig.Reset();
}

void LightTreeManager::ensureCapacity(ToFreeList& toFreeList, uint32_t sparseCount)
{
    if (sparseCount <= this->capacity && this->dev_lightAux != nullptr)
    {
        return;
    }

    const uint32_t newCapacity = nextCapacity(sparseCount);

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
    // Pass 1 — clear the full capacity to sentinel/zero
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
    // Pass 2 — fill live sparse slots from the scene
    // -------------------------------------------------
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

} // namespace Renderer
