// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "pipeline_builder.h"
#include "renderer_internal.h"
#include "rendering/buffer/buffer_helper.h"
#include "settings_manager.h"
#include "shaders.h"
#include <algorithm>
#include <cmath>

namespace Renderer
{
void sharcInit()
{
    auto& s = renderState.sharc;
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options{};
    // SM 6.9 is already required by initDevice. SM 6.6 guarantees int64 atomics
    // on root-descriptor structured buffers (we do not use typed/bindless atomics).
    s.supported =
        SUCCEEDED(renderState.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options, sizeof(options))) &&
        options.Native16BitShaderOpsSupported;
    if (!s.supported)
    {
        Logger::log("SHARC unavailable; using reference path tracer");
        return;
    }
    D3D12_ROOT_PARAMETER1 params[] = {
        MAKE_PARAM(CBV, COMMON, GLOBAL_PARAMS),
        MAKE_PARAM(UAV, SHARC, HASHES),
        MAKE_PARAM(UAV, SHARC, ACCUMULATION),
        MAKE_PARAM(UAV, SHARC, RESOLVED),
        MAKE_PARAM(UAV, SHARC, STATS),
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
          .Constants = { SHARC_REGISTER_CONTROL, SHARC_REGISTER_SPACE, 1 } },
    };
    serializeAndCreateRootSignature(params, std::size(params), nullptr, 0, s.computeRootSig);
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = s.computeRootSig.Get();
    desc.CS = makeShaderBytecode(getShader("sharc_maintenance_cs"));
    CHECK_HRESULT(renderState.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&s.maintenancePso)));
}

void sharcPrepare(ParamBlockManager& params, bool sceneChanged)
{
    auto& s = renderState.sharc;
    auto& p = *params.sharcParams;
    p = {};
    const bool selfTest = SettingsManager::getAsBool("sharcSelfTest");
    p.enabled = s.supported && (SettingsManager::getAsBool("sharc") || selfTest);
    if (!p.enabled)
    {
        s.wasEnabled = false;
        return;
    }
    p.capacity = 1u << SettingsManager::getAsUint("sharcCapacityLog2");
    p.downscale = SettingsManager::getAsUint("sharcDownscale");
    p.sceneScale = SettingsManager::getAsFloat("sharcSceneScale");
    p.roughnessMin = SettingsManager::getAsFloat("sharcRoughnessMin");
    p.accumulationFrames = SettingsManager::getAsUint("sharcAccumulationFrames");
    p.staleFrames = SettingsManager::getAsUint("sharcStaleFrames");
    p.debugMode = SettingsManager::getAsUint("sharcDebug");
    p.diagnostics = SettingsManager::getAsBool("sharcDiagnostics") || selfTest;
    bool reset = sceneChanged || s.resetRequested || !s.wasEnabled ||
                 s.previousScale != p.sceneScale;
    auto& freeList = renderState.frameCtxs[renderState.frameCtxIdx].toFreeList;
    if (s.capacity != p.capacity)
    {
        const auto allocate = [&](ComPtr<ID3D12Resource>& buffer, uint64_t size, const wchar_t* name)
        {
            if (buffer)
                freeList.pushResource(buffer);
            buffer = BufferHelper::createBasicBuffer(size,
                                                     &DEFAULT_HEAP,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
            buffer->SetName(name);
        };
        allocate(s.hashes, uint64_t(p.capacity) * 8, L"SHARC hashes");
        allocate(s.accumulation, uint64_t(p.capacity) * 16, L"SHARC accumulation");
        allocate(s.resolved, uint64_t(p.capacity) * 16, L"SHARC resolved");
        if (!s.stats)
        {
            allocate(s.stats, sizeof(s.lastStats), L"SHARC counters");
            for (auto& buffer : s.readback)
                buffer = BufferHelper::createBasicBuffer(sizeof(s.lastStats), &READBACK_HEAP);
        }
        s.capacity = p.capacity;
        reset = true;
    }
    const auto& camera = *params.cameraParams;
    const auto offset = camera.globalInstanceOffset;
    const auto delta = DirectX::XMINT3(offset.x - s.origin.x, offset.y - s.origin.y, offset.z - s.origin.z);
    // Keep hash coordinates stable across ordinary renderer-origin shifts. Re-anchor
    // rarely, before packed grid coordinates or floating point precision become unsafe.
    if (reset || std::abs(delta.x) > 2048 || std::abs(delta.y) > 2048 || std::abs(delta.z) > 2048)
    {
        s.origin = offset;
        reset = true;
    }
    p.originDelta = { offset.x - s.origin.x, offset.y - s.origin.y, offset.z - s.origin.z };
    p.cameraPosition = { camera.pos_WS.x + p.originDelta.x,
                         camera.pos_WS.y + p.originDelta.y,
                         camera.pos_WS.z + p.originDelta.z };
    p.cameraPositionPrev = reset ? p.cameraPosition : s.previousCamera;
    if (reset)
        s.frameIndex = 0;
    p.frameIndex = s.frameIndex;
    s.resetRequested = reset;
    s.previousScale = p.sceneScale;
    s.wasEnabled = true;
}

void sharcMaintenance(ParamBlockManager& params, SharcMaintenanceMode mode)
{
    auto& s = renderState.sharc;
    auto* cmd = renderState.cmdList.Get();
    cmd->SetPipelineState(s.maintenancePso.Get());
    cmd->SetComputeRootSignature(s.computeRootSig.Get());
    cmd->SetComputeRootConstantBufferView(0, params.getParamBufferGpuAddress());
    cmd->SetComputeRootUnorderedAccessView(1, s.hashes->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(2, s.accumulation->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, s.resolved->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(4, s.stats->GetGPUVirtualAddress());
    cmd->SetComputeRoot32BitConstant(5, mode, 0);
    cmd->Dispatch(mode == SHARC_MAINTENANCE_TEST_INSERT || mode == SHARC_MAINTENANCE_TEST_QUERY || mode == SHARC_MAINTENANCE_TEST_MISS || mode == SHARC_MAINTENANCE_RESET_STATS ? 1 : (s.capacity + 255) / 256, 1, 1);
    // All cache accesses stay in UAV state. Ordering covers clears, update atomics,
    // temporal resolve, and the subsequent read-only query in the path tracer.
    BufferHelper::uavBarrier(cmd, nullptr);
}

void sharcBindPt()
{
    const auto& s = renderState.sharc;
    auto* cmd = renderState.cmdList.Get();
    cmd->SetComputeRootUnorderedAccessView(PT_PARAM_IDX(SHARC_HASHES), s.hashes->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(PT_PARAM_IDX(SHARC_ACCUMULATION), s.accumulation->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(PT_PARAM_IDX(SHARC_RESOLVED), s.resolved->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(PT_PARAM_IDX(SHARC_STATS), s.stats->GetGPUVirtualAddress());
}

void sharcReadStats(uint32_t slot)
{
    auto& s = renderState.sharc;
    if (!s.readbackPending[slot])
        return;
    void* data = nullptr;
    D3D12_RANGE range{ 0, sizeof(s.lastStats) };
    CHECK_HRESULT(s.readback[slot]->Map(0, &range, &data));
    memcpy(s.lastStats.data(), data, sizeof(s.lastStats));
    D3D12_RANGE empty{ 0, 0 };
    s.readback[slot]->Unmap(0, &empty);
    s.readbackPending[slot] = false;
    if (renderState.frameNumber % 60 == 0)
        Logger::log("SHARC queries=%u hits=%u bounces=%u updates=%u failed=%u occupied=%u primaryRays=%u primaryEmitterHits=%u primaryGlass=%u primaryGlassRays=%u primaryGlassCacheHits=%u primaryGlassQueried=%u",
                    s.lastStats[SHARC_COUNTER_QUERIES],
                    s.lastStats[SHARC_COUNTER_HITS],
                    s.lastStats[SHARC_COUNTER_BOUNCES],
                    s.lastStats[SHARC_COUNTER_UPDATES],
                    s.lastStats[SHARC_COUNTER_FAILED_INSERTS],
                    s.lastStats[SHARC_COUNTER_OCCUPIED],
                    s.lastStats[SHARC_COUNTER_PRIMARY_RAYS],
                    s.lastStats[SHARC_COUNTER_PRIMARY_EMITTER_HITS],
                    s.lastStats[SHARC_COUNTER_PRIMARY_GLASS],
                    s.lastStats[SHARC_COUNTER_PRIMARY_GLASS_RAYS],
                    s.lastStats[SHARC_COUNTER_PRIMARY_GLASS_CACHE_HITS],
                    s.lastStats[SHARC_COUNTER_PRIMARY_GLASS_QUERIED]);
}

void sharcCopyStats()
{
    auto& s = renderState.sharc;
    const auto slot = renderState.frameCtxIdx;
    auto* cmd = renderState.cmdList.Get();
    BufferHelper::stateTransitionResourceBarrier(
        cmd, s.stats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(s.readback[slot].Get(), 0, s.stats.Get(), 0, sizeof(s.lastStats));
    BufferHelper::stateTransitionResourceBarrier(
        cmd, s.stats.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s.readbackPending[slot] = true;
}

void sharcDestroy()
{
    renderState.sharc = {};
}
} // namespace Renderer
