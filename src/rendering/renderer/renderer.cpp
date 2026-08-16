// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include "rendering/camera.h"
#include "rendering/window_manager.h"
#include "rendering/buffer/acs_helper.h"
#include "rendering/buffer/buffer_helper.h"
#include "rendering/common/common_enums.h"
#include "rendering/common/common_settings.h"
#include "rendering/biome_map.h"
#include "rendering/sky_atmosphere.h"
#include "rendering/water_displacer.h"
#include "scene/gltf_loader.h"
#include "scene/scene.h"
#include "terrain/terrain.h"
#include "terrain/terrain_omm.h"
#include "util/math.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "NrcD3d12.h"
#undef min
#undef max

#include "settings_manager.h"
#include "logger.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#include <implot.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_d.h>

using namespace DirectX;

using WindowManager::hwnd;

namespace Renderer
{

static constexpr float defaultFovYDegrees = 35;

static constexpr float timeScrubSpeed = 50.f; // anim time multiplier while a bracket key is held

void init()
{
    renderState.testMode = SettingsManager::isTestMode();
    renderState.voxelMode = SettingsManager::getAsBool("voxelMode");
    renderState.animTime = SettingsManager::getAsFloat("animTime");

    initStreamline();

    initDevice();
    initDescriptorHeaps();

    initNvapi();

    for (uint32_t frameIdx = 0; frameIdx < NUM_FRAMES_IN_FLIGHT; ++frameIdx)
    {
        FrameContext& frameCtx = renderState.frameCtxs[frameIdx];
        frameCtx.paramBlockManager.init();
        frameCtx.paramBlockManager.setName(L"paramBlockManager " + std::to_wstring(frameIdx));

        frameCtx.paramBlockManager.sceneParams->voxelMode = renderState.voxelMode ? 1 : 0;
    }

    renderState.useWaitableSwapChain = SettingsManager::getAsBool("useWaitableSwapChain");

    initSwapChain();
    SkyAtmosphere::init();
    for (auto& frame : renderState.frameCtxs)
    {
        frame.paramBlockManager.heapIndices->srv.transmittanceLutIdx = SkyAtmosphere::getTransmittanceLutSrvIdx();
        frame.paramBlockManager.heapIndices->srv.skyViewLutIdx = SkyAtmosphere::getSkyViewLutSrvIdx();
    }
    initRtTargets();
    initCommand();
    initConstantParams();

    renderState.camera.init(XMConvertToRadians(defaultFovYDegrees));

    AcsHelper::init();

    renderState.scene.init();

    initRootSignature();
    initPipeline();

    WaterDisplacer::init();

    renderState.lightTreeManager.init();
    renderState.gpuRadixSort.init();

    initImgui();

    const std::string& defaultScene = SettingsManager::getAsString("scene");
    if (renderState.voxelMode)
    {
        Terrain::init(&renderState.scene);
        Terrain::importWorld();
    }
    else
    {
        if (!defaultScene.empty())
        {
            loadScene(defaultScene);
        }
    }

    if (SettingsManager::getAsBool("nrcEnabled"))
    {
        initNrc();
    }

    if (!renderState.testMode)
    {
        SetForegroundWindow(hwnd);
    }
}

void loadScene(const std::string& filePathStr)
{
    flush();
    GltfLoader::loadGltf(filePathStr, renderState.scene);
    if (renderState.nrcContext != nullptr)
    {
        configureNrc();
    }
    renderState.dlss.needsReset = true;
}


static const std::vector<sl::DLSSMode> dlssModes = {
    sl::DLSSMode::eDLAA,
    sl::DLSSMode::eMaxQuality,
    sl::DLSSMode::eBalanced,
    sl::DLSSMode::eMaxPerformance,
    sl::DLSSMode::eUltraPerformance,
};

void resize()
{
    if (!renderState.swapChain)
    {
        return;
    }

    renderState.frameNumber = 0;

    RECT rect;
    GetClientRect(hwnd, &rect);
    const uint32_t viewportWidth = std::max<uint32_t>(rect.right - rect.left, 1);
    const uint32_t viewportHeight = std::max<uint32_t>(rect.bottom - rect.top, 1);

    renderState.viewport = { 0, 0, static_cast<float>(viewportWidth), static_cast<float>(viewportHeight) };
    renderState.scissor = { 0, 0, static_cast<long>(viewportWidth), static_cast<long>(viewportHeight) };

    const AntialiasingMode antialiasingMode =
        static_cast<AntialiasingMode>(SettingsManager::getAsUint("antialiasingMode"));
    if (antialiasingMode == AntialiasingMode::DLSS)
    {
        renderState.dlss.viewportExtent = { 0, 0, viewportWidth, viewportHeight };

        sl::DLSSDOptimalSettings dlssdSettings;
        renderState.dlss.options.mode = (sl::DLSSMode)dlssModes[SettingsManager::getAsUint("dlssMode")];
        renderState.dlss.options.outputWidth = viewportWidth;
        renderState.dlss.options.outputHeight = viewportHeight;
        CHECK_SL_RESULT(slDLSSDGetOptimalSettings(renderState.dlss.options, dlssdSettings));

        renderState.renderWidth = dlssdSettings.optimalRenderWidth;
        renderState.renderHeight = dlssdSettings.optimalRenderHeight;
        renderState.dlss.mipBias = std::log2(static_cast<float>(renderState.renderWidth) / static_cast<float>(viewportWidth)) - 1.f;

        renderState.dlss.renderExtent = { 0, 0, renderState.renderWidth, renderState.renderHeight };

        renderState.dlss.options.dlaaPreset = sl::DLSSDPreset::ePresetD;
        renderState.dlss.options.qualityPreset = sl::DLSSDPreset::ePresetD;
        renderState.dlss.options.balancedPreset = sl::DLSSDPreset::ePresetD;
        renderState.dlss.options.performancePreset = sl::DLSSDPreset::ePresetD;
        renderState.dlss.options.ultraPerformancePreset = sl::DLSSDPreset::ePresetD;
        renderState.dlss.options.colorBuffersHDR = sl::Boolean::eTrue;
        renderState.dlss.options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
        renderState.dlss.options.alphaUpscalingEnabled = sl::Boolean::eFalse;
        CHECK_SL_RESULT(slDLSSDSetOptions(renderState.dlss.viewportHandle, renderState.dlss.options));

        renderState.dlss.needsReset = true;
    }
    else
    {
        renderState.renderWidth = viewportWidth;
        renderState.renderHeight = viewportHeight;
        renderState.dlss.mipBias = 0.f;
    }

    flush();

    CHECK_HRESULT(renderState.proxySwapChain->ResizeBuffers(0, viewportWidth, viewportHeight, DXGI_FORMAT_UNKNOWN, renderState.swapChainFlags));
    if (renderState.useWaitableSwapChain)
    {
        CHECK_HRESULT(renderState.swapChain->SetMaximumFrameLatency(2));
        if (renderState.frameLatencyWaitable)
        {
            CloseHandle(renderState.frameLatencyWaitable);
        }
        renderState.frameLatencyWaitable = renderState.swapChain->GetFrameLatencyWaitableObject();
    }

    renderState.dev_gbuffer.Reset();
    renderState.dev_gbuffer = BufferHelper::createBasicBuffer(renderState.renderWidth * renderState.renderHeight * sizeof(GbufferData),
                                                              &DEFAULT_HEAP,
                                                              { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    renderState.dev_gbuffer->SetName(L"dev_gbuffer");

    renderState.dev_pathTracingRawBuffer.Reset();
    const bool doPathSplitting = SettingsManager::getAsBool("doPathSplitting");
    const uint32_t pathTracingRawBufferSizeBytes =
        renderState.renderWidth * renderState.renderHeight * (doPathSplitting ? 2 : 1) * sizeof(float) * 4;
    renderState.dev_pathTracingRawBuffer = BufferHelper::createBasicBuffer(
        pathTracingRawBufferSizeBytes, &DEFAULT_HEAP, { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    renderState.dev_pathTracingRawBuffer->SetName(L"dev_pathTracingRawBuffer");

    renderState.dev_ptDiffuseAlbedoRawBuffer.Reset();
    renderState.dev_ptDiffuseAlbedoRawBuffer = BufferHelper::createBasicBuffer(
        pathTracingRawBufferSizeBytes, &DEFAULT_HEAP, { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    renderState.dev_ptDiffuseAlbedoRawBuffer->SetName(L"dev_ptDiffuseAlbedoRawBuffer");

    const uint32_t rtvIncrementSize = renderState.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (uint32_t frameIdx = 0; frameIdx < NUM_FRAMES_IN_FLIGHT; ++frameIdx)
    {
        ComPtr<ID3D12Resource> backBuffer;
        CHECK_HRESULT(renderState.proxySwapChain->GetBuffer(frameIdx, IID_PPV_ARGS(&backBuffer)));
        D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle = renderState.rtvHeapCpuHandles[frameIdx];
        cpuHandle = renderState.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += frameIdx * rtvIncrementSize;
        renderState.device->CreateRenderTargetView(backBuffer.Get(), nullptr, cpuHandle);
        const std::wstring backBufferName = L"backBuffer " + std::to_wstring(frameIdx);
        backBuffer->SetName(backBufferName.c_str());
    }

    for (RtTarget* rtTarget : renderState.allRtTargets)
    {
        rtTarget->reset();
        if (rtTarget->isFullSize)
        {
            rtTarget->setDimensions(viewportWidth, viewportHeight);
        }
        else
        {
            rtTarget->setDimensions(renderState.renderWidth, renderState.renderHeight);
        }
        rtTarget->init();
    }

    renderState.nrcDebugTarget.reset();
    renderState.nrcDebugTarget.setDimensions(renderState.renderWidth * (doPathSplitting ? 2 : 1), renderState.renderHeight);
    renderState.nrcDebugTarget.init();

    for (auto& frame : renderState.frameCtxs)
    {
        auto& uav = frame.paramBlockManager.heapIndices->uav;
        uav.pathTracingTargetIdx = renderState.pathTracingTarget.getUavIdx();
        uav.diffuseAlbedoTargetIdx = renderState.diffuseAlbedoTarget.getUavIdx();
        uav.specularAlbedoTargetIdx = renderState.specularAlbedoTarget.getUavIdx();
        uav.linearDepthTargetIdx = renderState.linearDepthTarget.getUavIdx();

        uav.normalsAndRoughnessTargetIdx = renderState.normalsAndRoughnessTarget.getUavIdx();
        uav.motionTargetIdx = renderState.motionTarget.getUavIdx();
        uav.specularHitDistanceTargetIdx = renderState.specularHitDistanceTarget.getUavIdx();
        uav.debugTargetIdx = renderState.debugTarget.getUavIdx();

        auto& srv = frame.paramBlockManager.heapIndices->srv;
        srv.pathTracingTargetIdx = renderState.pathTracingTarget.getSrvIdx();
        srv.diffuseAlbedoTargetIdx = renderState.diffuseAlbedoTarget.getSrvIdx();
        srv.specularAlbedoTargetIdx = renderState.specularAlbedoTarget.getSrvIdx();
        srv.linearDepthTargetIdx = renderState.linearDepthTarget.getSrvIdx();

        srv.normalsAndRoughnessTargetIdx = renderState.normalsAndRoughnessTarget.getSrvIdx();
        srv.motionTargetIdx = renderState.motionTarget.getSrvIdx();
        srv.specularHitDistanceTargetIdx = renderState.specularHitDistanceTarget.getSrvIdx();
        srv.dlssOutputTargetIdx = renderState.dlssOutputTarget.getSrvIdx();

        srv.debugTargetIdx = renderState.debugTarget.getSrvIdx();
    }

    renderState.camera.setAspectRatio(static_cast<float>(renderState.renderWidth) / static_cast<float>(renderState.renderHeight));

    // DLSS programming guide says to use this as the jitter sequence length:
    // Total Phases = Base Phase Count * (Target Resolution / Render Resolution) ^ 2
    //
    // Streamline programming guide says there's no reason to limit the sequence length, so I'm using 64 for the "Base
    // Phase Count" instead of the default/recommended of 8.
    const float dlssScaleFactor = static_cast<float>(viewportWidth) / static_cast<float>(renderState.renderWidth);
    const uint32_t jitterHaltonSequenceLength = SettingsManager::getAsBool("noJitter")
        ? 0u
        : static_cast<uint32_t>(ceilf(64 * (dlssScaleFactor * dlssScaleFactor)));
    renderState.camera.setJitterHaltonSequenceLength(jitterHaltonSequenceLength);

    renderState.frameTimeBuffer.clear();

    if (renderState.nrcContext != nullptr)
    {
        // TODO: try to avoid fully destroying and recreating NRC on resize
        destroyNrc();
        initNrc();
    }
}

void queueResize()
{
    renderState.needsResize = true;
}

// state = state the resource should be in when SL (DLSS) is invoked
static inline sl::Resource makeSlResource(RtTarget* target,
                                          D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
{
    return {
        sl::ResourceType::eTex2d,
        target->getTarget(),
        static_cast<uint32_t>(state),
    };
}

static void bindSceneSrvs(uint32_t baseIdx)
{
    renderState.cmdList->SetComputeRootShaderResourceView(baseIdx + 0, renderState.scene.getDevTlasAddress());
    renderState.cmdList->SetComputeRootShaderResourceView(baseIdx + 1, renderState.scene.getDevVertsBufferAddress());
    renderState.cmdList->SetComputeRootShaderResourceView(baseIdx + 2, renderState.scene.getDevIdxsBufferAddress());
    renderState.cmdList->SetComputeRootShaderResourceView(baseIdx + 3, renderState.scene.getDevInstanceDatasAddress());
    renderState.cmdList->SetComputeRootShaderResourceView(baseIdx + 4, renderState.scene.getDevMaterialsAddress());
    renderState.cmdList->SetComputeRootShaderResourceView(baseIdx + 5, renderState.scene.getDevPerTriDatasBufferAddress());
    renderState.cmdList->SetComputeRootShaderResourceView(baseIdx + 6, renderState.scene.getDevAreaLightsBufferAddress());
    renderState.cmdList->SetComputeRootShaderResourceView(baseIdx + 7, renderState.scene.getDevAreaLightSamplingStructureAddress());
}

static void bindNrcBuffers(const nrc::d3d12::Buffers* nrcBuffers, uint32_t baseIdx)
{
    renderState.cmdList->SetComputeRootUnorderedAccessView(baseIdx + 0, (*nrcBuffers)[nrc::BufferIdx::QueryPathInfo].resource->GetGPUVirtualAddress());
    renderState.cmdList->SetComputeRootUnorderedAccessView(baseIdx + 1, (*nrcBuffers)[nrc::BufferIdx::TrainingPathInfo].resource->GetGPUVirtualAddress());
    renderState.cmdList->SetComputeRootUnorderedAccessView(baseIdx + 2, (*nrcBuffers)[nrc::BufferIdx::TrainingPathVertices].resource->GetGPUVirtualAddress());
    renderState.cmdList->SetComputeRootUnorderedAccessView(baseIdx + 3, (*nrcBuffers)[nrc::BufferIdx::QueryRadianceParams].resource->GetGPUVirtualAddress());
    renderState.cmdList->SetComputeRootUnorderedAccessView(baseIdx + 4, (*nrcBuffers)[nrc::BufferIdx::Counter].resource->GetGPUVirtualAddress());
}

static void bindPtCommonParams(ParamBlockManager& paramBlockManager)
{
    renderState.cmdList->SetComputeRootConstantBufferView(PT_PARAM_IDX(GLOBAL_PARAMS), paramBlockManager.getParamBufferGpuAddress());
    bindSceneSrvs(PT_PARAM_IDX(RAYTRACING_ACS));
    renderState.cmdList->SetComputeRootShaderResourceView(PT_PARAM_IDX(GBUFFER_IN), renderState.dev_gbuffer->GetGPUVirtualAddress());
    renderState.cmdList->SetComputeRootShaderResourceView(PT_PARAM_IDX(RTSL_LIGHT_TREE),
                                                          renderState.lightTreeManager.getDevLightTreeSrvBindAddress());
    renderState.cmdList->SetComputeRootShaderResourceView(PT_PARAM_IDX(RTSL_LIGHT_TO_LEAF),
                                                          renderState.lightTreeManager.getDevLightToLeafSrvBindAddress());
}

static void dispatchPathTracing(ParamBlockManager& paramBlockManager, bool doPathSplitting)
{
    // ===================================
    // NRC UPDATE
    // ===================================

    if (renderState.nrcContext != nullptr)
    {
        renderState.cmdList->SetPipelineState1(renderState.nrcUpdatePso.Get());
        renderState.cmdList->SetComputeRootSignature(renderState.ptRootSig.Get());

        bindPtCommonParams(paramBlockManager);
        renderState.cmdList->SetComputeRootConstantBufferView(PT_PARAM_IDX(NRC_CONSTANTS), paramBlockManager.getNrcConstantsGpuAddress());
        bindNrcBuffers(renderState.nrcContext->GetBuffers(), PT_PARAM_IDX(NRC_QUERY_PATH_INFO));

        renderState.nrcUpdateDispatchDesc.Width = paramBlockManager.nrcConstants->trainingDimensions.x;
        renderState.nrcUpdateDispatchDesc.Height = paramBlockManager.nrcConstants->trainingDimensions.y;
        renderState.cmdList->DispatchRays(&renderState.nrcUpdateDispatchDesc);

        BufferHelper::uavBarrier(renderState.cmdList.Get(), nullptr);
    }

    // ===================================
    // PATH TRACING (or NRC QUERY)
    // ===================================

    const bool useNrcQuery = (renderState.nrcContext != nullptr);
    renderState.cmdList->SetPipelineState1(useNrcQuery ? renderState.nrcQueryPso.Get() : renderState.ptPso.Get());
    renderState.cmdList->SetComputeRootSignature(renderState.ptRootSig.Get());

    bindPtCommonParams(paramBlockManager);

    renderState.cmdList->SetComputeRootUnorderedAccessView(PT_PARAM_IDX(PATH_TRACING_RAW_BUFFER_OUT), renderState.dev_pathTracingRawBuffer->GetGPUVirtualAddress());
    renderState.cmdList->SetComputeRootUnorderedAccessView(PT_PARAM_IDX(PT_DIFFUSE_ALBEDO_RAW_BUFFER_OUT), renderState.dev_ptDiffuseAlbedoRawBuffer->GetGPUVirtualAddress());

    if (useNrcQuery)
    {
        renderState.cmdList->SetComputeRootConstantBufferView(PT_PARAM_IDX(NRC_CONSTANTS), paramBlockManager.getNrcConstantsGpuAddress());
        bindNrcBuffers(renderState.nrcContext->GetBuffers(), PT_PARAM_IDX(NRC_QUERY_PATH_INFO));
    }

    D3D12_DISPATCH_RAYS_DESC& activePtDispatchDesc = useNrcQuery ? renderState.nrcQueryDispatchDesc : renderState.ptDispatchDesc;
    activePtDispatchDesc.Width = renderState.gbufferDispatchDesc.Width * (doPathSplitting ? 2 : 1);
    activePtDispatchDesc.Height = renderState.gbufferDispatchDesc.Height;
    renderState.cmdList->DispatchRays(&activePtDispatchDesc);

    // ===================================
    // NRC RESOLVE
    // ===================================

    if (useNrcQuery)
    {
        BufferHelper::uavBarrier(renderState.cmdList.Get(), nullptr);

        renderState.nrcContext->QueryAndTrain(renderState.cmdList.Get(), nullptr);

        ID3D12DescriptorHeap* const descHeaps[] = { renderState.sharedDescriptorHeap.Get() };
        renderState.cmdList->SetDescriptorHeaps(std::size(descHeaps), descHeaps);
        BufferHelper::uavBarrier(renderState.cmdList.Get(), nullptr);

        const NrcResolveMode resolveMode = static_cast<NrcResolveMode>(SettingsManager::getAsUint("nrcResolveMode"));
        const bool useBuiltinResolve = (resolveMode != NrcResolveMode::AddQueryResultToOutput);

        if (useBuiltinResolve)
        {
            renderState.nrcDebugTarget.transitionToState(renderState.cmdList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            renderState.nrcContext->Resolve(renderState.cmdList.Get(), renderState.nrcDebugTarget.getTarget());
            renderState.cmdList->SetDescriptorHeaps(std::size(descHeaps), descHeaps);
        }
        else
        {
            const nrc::d3d12::Buffers* nrcBuffers = renderState.nrcContext->GetBuffers();
            renderState.cmdList->SetPipelineState(renderState.nrcResolvePso.Get());
            renderState.cmdList->SetComputeRootSignature(renderState.nrcResolveRootSig.Get());

            renderState.cmdList->SetComputeRootConstantBufferView(NRC_RESOLVE_PARAM_IDX(NRC_CONSTANTS), paramBlockManager.getNrcConstantsGpuAddress());
            renderState.cmdList->SetComputeRootUnorderedAccessView(
                NRC_RESOLVE_PARAM_IDX(QUERY_PATH_INFO),
                (*nrcBuffers)[nrc::BufferIdx::QueryPathInfo].resource->GetGPUVirtualAddress());
            renderState.cmdList->SetComputeRootUnorderedAccessView(
                NRC_RESOLVE_PARAM_IDX(QUERY_RADIANCE),
                (*nrcBuffers)[nrc::BufferIdx::QueryRadiance].resource->GetGPUVirtualAddress());
            renderState.cmdList->SetComputeRootUnorderedAccessView(
                NRC_RESOLVE_PARAM_IDX(PATH_TRACING_RAW_BUFFER_OUT),
                renderState.dev_pathTracingRawBuffer->GetGPUVirtualAddress());

            const uint32_t nrcResolveDispatchWidth =
                Util::calculateDispatchSize(paramBlockManager.nrcConstants->frameDimensions.x, NRC_RESOLVE_WORKGROUP_SIZE_X);
            const uint32_t nrcResolveDispatchHeight =
                Util::calculateDispatchSize(paramBlockManager.nrcConstants->frameDimensions.y, NRC_RESOLVE_WORKGROUP_SIZE_Y);
            renderState.cmdList->Dispatch(nrcResolveDispatchWidth, nrcResolveDispatchHeight, 1);
        }

        BufferHelper::uavBarrier(renderState.cmdList.Get(), nullptr);
    }
}

static void beginFrame();
static void submitCmd();

// Fog strength peaks around sunrise and sunset: full within fogFullStrengthSeconds of the sun
// crossing the horizon, fading to zero with smoothstep by fogFadeEndSeconds away.
static constexpr float fogPeakSigmaS = 0.004f;
static constexpr float fogFullStrengthSeconds = 30.f;
static constexpr float fogFadeEndSeconds = 120.f;

static float computeFogSigmaS(const float animTime)
{
    float dayTime = std::fmod(animTime, SUN_PERIOD_SECONDS);
    if (dayTime < 0.f)
    {
        dayTime += SUN_PERIOD_SECONDS;
    }
    const float distToSunrise = std::min(dayTime, SUN_PERIOD_SECONDS - dayTime);
    const float distToSunset = std::abs(dayTime - 0.5f * SUN_PERIOD_SECONDS);
    const float dist = std::min(distToSunrise, distToSunset);
    const float ramp = 1.f - glm::smoothstep(fogFullStrengthSeconds, fogFadeEndSeconds, dist);
    return fogPeakSigmaS * ramp * SettingsManager::getAsFloat("fogScatteringMultiplier");
}

void render()
{
    if (renderState.needsResize)
    {
        resize();
        renderState.needsResize = false;
    }

    const bool showGui = SettingsManager::getAsBool("showGui");
    if (showGui)
    {
        imguiBeginFrame();
    }

    const auto currentTimePoint = std::chrono::high_resolution_clock::now();
    const double deltaTime = std::chrono::duration<double>(currentTimePoint - renderState.lastTimePoint).count();
    renderState.lastTimePoint = currentTimePoint;
    // Scrubbing overrides the pause, so time can be stepped from a frozen scene
    const float timeScrubDirection = WindowManager::getTimeScrubDirection();
    const float animTimeScale = timeScrubDirection != 0.f
        ? timeScrubDirection * timeScrubSpeed
        : (SettingsManager::getAsBool("animTimePaused") ? 0.f : 1.f);
    renderState.animTime += deltaTime * animTimeScale;
    // TODO: float precision of elapsed seconds degrades after hours (~1 ms resolution at ~4.6 h);
    // wave phase gets steppy in long sessions. Wrap time periodically if it matters.
    const float animTimeFloat = static_cast<float>(renderState.animTime);

    beginFrame();

    const AntialiasingMode antialiasingMode =
        static_cast<AntialiasingMode>(SettingsManager::getAsUint("antialiasingMode"));
    const bool useDlss = antialiasingMode == AntialiasingMode::DLSS;

    sl::FrameToken* frameToken;
    sl::Constants slConstants;
    if (useDlss)
    {
        CHECK_SL_RESULT(slGetNewFrameToken(frameToken));

        {
            // clang-format off
            sl::Resource pathTracingResource = makeSlResource(&renderState.pathTracingTarget);
            sl::Resource dlssOutputResource = makeSlResource(&renderState.dlssOutputTarget);
            sl::Resource linearDepthResource = makeSlResource(&renderState.linearDepthTarget);
            sl::Resource motionResource = makeSlResource(&renderState.motionTarget);
            sl::Resource diffuseAlbedoResource = makeSlResource(&renderState.diffuseAlbedoTarget);
            sl::Resource specularAlbedoResource = makeSlResource(&renderState.specularAlbedoTarget);
            sl::Resource normalsAndRoughnessResource = makeSlResource(&renderState.normalsAndRoughnessTarget);
            sl::Resource specularHitDistanceResource = makeSlResource(&renderState.specularHitDistanceTarget);

            sl::ResourceTag resourceTags[] = {
                {&pathTracingResource, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &renderState.dlss.renderExtent},
                {&dlssOutputResource, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &renderState.dlss.viewportExtent},
                {&linearDepthResource, sl::kBufferTypeLinearDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderState.dlss.renderExtent},
                {&motionResource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderState.dlss.renderExtent},
                {&diffuseAlbedoResource, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &renderState.dlss.renderExtent},
                {&specularAlbedoResource, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &renderState.dlss.renderExtent},
                {&normalsAndRoughnessResource, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilPresent, &renderState.dlss.renderExtent},
                {&specularHitDistanceResource, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eValidUntilPresent, &renderState.dlss.renderExtent},
            };
            // clang-format on

            CHECK_SL_RESULT(
                slSetTagForFrame(*frameToken, renderState.dlss.viewportHandle, resourceTags, _countof(resourceTags), renderState.cmdList.Get()));
        }

        slConstants = {};
        slConstants.depthInverted = sl::Boolean::eFalse;
        slConstants.cameraMotionIncluded = sl::Boolean::eTrue;
        slConstants.motionVectors3D = sl::Boolean::eFalse;
        slConstants.orthographicProjection = sl::Boolean::eFalse;
        slConstants.motionVectorsDilated = sl::Boolean::eFalse;
        slConstants.motionVectorsJittered = sl::Boolean::eFalse;

        if (renderState.dlss.needsReset)
        {
            slConstants.reset = sl::Boolean::eTrue;
            renderState.dlss.needsReset = false;
        }
        else
        {
            slConstants.reset = sl::Boolean::eFalse;
        }
    }

    auto& frameCtx = renderState.frameCtxs[renderState.frameCtxIdx];

    ParamBlockManager& paramBlockManager = frameCtx.paramBlockManager;

    PlayerInput playerInput = {};
    if (!SettingsManager::getAsBool("lockCamera"))
    {
        playerInput = WindowManager::getPlayerInput();
    }
    renderState.camera.processInput(deltaTime, playerInput);

    if (renderState.voxelMode)
    {
        TerrainOmm::buildArrayIfPending(renderState.cmdList.Get(), frameCtx.toFreeList);
        Terrain::update(frameCtx.toFreeList);
        BiomeMap::update(renderState.cmdList.Get(), frameCtx.toFreeList);
    }

    const bool didSceneChange = renderState.scene.update(renderState.cmdList.Get(), frameCtx.toFreeList, animTimeFloat);

    const bool didCameraChange = renderState.camera.update();

    if (useDlss)
    {
        renderState.camera.copySlConstantsTo(&slConstants);
        CHECK_SL_RESULT(slSetConstants(slConstants, *frameToken, renderState.dlss.viewportHandle));

        renderState.camera.copyMatricesToDlssOptions(&renderState.dlss.options.worldToCameraView, &renderState.dlss.options.cameraViewToWorld);
        CHECK_SL_RESULT(slDLSSDSetOptions(renderState.dlss.viewportHandle, renderState.dlss.options));
    }

    renderState.camera.copyParamsTo(paramBlockManager.cameraParams);

    const bool resetAccumulation = didCameraChange || didSceneChange || renderState.didPathTracingSettingsChange;

    auto& renderParams = paramBlockManager.renderParams;
    renderParams->frameNumber = renderState.frameNumber;
    renderParams->animTime = animTimeFloat;
    renderParams->prevAnimTime = renderState.prevAnimTime;
    renderState.prevAnimTime = animTimeFloat;

    const bool waitingForImport = renderState.testMode && renderState.voxelMode && !Terrain::pollTestModeImport();

    if (resetAccumulation)
    {
        renderState.accumulatedFrameNumber = 0;
        renderState.stopAccumulating = false;
    }
    else if (!renderState.stopAccumulating && !waitingForImport)
    {
        if (++renderState.accumulatedFrameNumber == SettingsManager::getAsUint("maxAccumulatedFrames"))
        {
            renderState.stopAccumulating = true;

            if (renderState.testMode)
            {
                queueScreenshot(true /*useTestOutputPath*/);
            }
        }
    }

    renderParams->accumulatedFrameNumber = renderState.accumulatedFrameNumber;
    renderParams->maxPathDepth = SettingsManager::getAsUint("maxPathDepth");
    renderParams->samplingMode = SettingsManager::getAsUint("samplingMode");
    renderParams->tonemapping = SettingsManager::getAsUint("tonemapping");
    renderParams->preTonemappedColorSrvIdx = useDlss ? renderState.dlssOutputTarget.getSrvIdx() : renderState.pathTracingTarget.getSrvIdx();
    renderParams->renderSize = { renderState.renderWidth, renderState.renderHeight };
    const bool doPathSplitting = SettingsManager::getAsBool("doPathSplitting");
    renderParams->doPathSplitting = doPathSplitting ? 1 : 0;
    renderParams->antialiasingMode = static_cast<uint32_t>(antialiasingMode);
    renderParams->refractionIndirectPassthrough = SettingsManager::getAsBool("refractionIndirectPassthrough") ? 1 : 0;
    renderParams->mipBias = renderState.dlss.mipBias;
    renderParams->fogSigmaS = computeFogSigmaS(animTimeFloat);
    renderParams->fogScaleHeight = SettingsManager::getAsFloat("fogScaleHeight");
    renderParams->fogG = SettingsManager::getAsFloat("fogG");
    renderParams->fogMarchSteps = SettingsManager::getAsUint("fogMarchSteps");
    renderParams->fogAmbientStrength = SettingsManager::getAsFloat("fogAmbientStrength");

    RtTarget* debugOutputTarget = nullptr;
    const std::string& debugViewSettingStr = SettingsManager::getAsString("debugView");
    if (renderState.debugViewComboMap.contains(debugViewSettingStr))
    {
        debugOutputTarget = renderState.debugViewComboMap.at(debugViewSettingStr);
    }

    auto& debugParams = paramBlockManager.debugParams;
    if (debugOutputTarget == nullptr)
    {
        debugParams->debugOutputSrvIdx = ~0u;
    }
    else
    {
        ASSERT(debugOutputTarget->debugOutputNumChannels > 0);
        debugParams->debugOutputSrvIdx = debugOutputTarget->getSrvIdx();
        debugParams->debugOutputNumChannels = debugOutputTarget->debugOutputNumChannels;
    }
    debugParams->debugOutputScale = SettingsManager::getAsFloat("debugViewScale");
    debugParams->debugViewApplyTonemap = SettingsManager::getAsBool("debugViewApplyTonemap") ? 1 : 0;

    if (renderState.voxelMode)
    {
        debugParams->colorChunks = SettingsManager::getAsBool("debugColorChunks") ? 1 : 0;
    }

    debugParams->debugBool0 = SettingsManager::getAsBool("debugBool0");
    debugParams->debugBool1 = SettingsManager::getAsBool("debugBool1");
    debugParams->debugBool2 = SettingsManager::getAsBool("debugBool2");
    debugParams->debugBool3 = SettingsManager::getAsBool("debugBool3");

    debugParams->debugFloat0 = SettingsManager::getAsFloat("debugFloat0");
    debugParams->debugFloat1 = SettingsManager::getAsFloat("debugFloat1");
    debugParams->debugFloat2 = SettingsManager::getAsFloat("debugFloat2");
    debugParams->debugFloat3 = SettingsManager::getAsFloat("debugFloat3");

    const bool nrcEnabled = SettingsManager::getAsBool("nrcEnabled");
    static bool nrcPrevEnabled = SettingsManager::getAsBool("nrcEnabled");
    if (nrcEnabled != nrcPrevEnabled)
    {
        if (nrcEnabled)
        {
            initNrc();
        }
        else
        {
            destroyNrc();
        }
    }
    nrcPrevEnabled = nrcEnabled;

    static uint32_t nrcPrevMaxPathDepth = 0;
    const uint32_t maxPathDepth = SettingsManager::getAsUint("maxPathDepth");
    if (renderState.nrcContext != nullptr && maxPathDepth != nrcPrevMaxPathDepth)
    {
        configureNrc();
    }
    nrcPrevMaxPathDepth = maxPathDepth;

    if (renderState.nrcContext != nullptr)
    {
        nrc::FrameSettings frameSettings;
        frameSettings.maxExpectedAverageRadianceValue = SettingsManager::getAsFloat("nrcMaxRadiance");
        frameSettings.terminationHeuristicThreshold = SettingsManager::getAsFloat("nrcTerminationThreshold");
        frameSettings.trainingTerminationHeuristicThreshold = SettingsManager::getAsFloat("nrcTrainingTerminationThreshold");
        frameSettings.resolveMode = static_cast<NrcResolveMode>(SettingsManager::getAsUint("nrcResolveMode"));
        frameSettings.skipDeltaVertices = SettingsManager::getAsBool("nrcSkipDeltaVertices");
        frameSettings.trainTheCache = SettingsManager::getAsBool("nrcTrainTheCache");
        frameSettings.learningRate = SettingsManager::getAsFloat("nrcLearningRate");
        renderState.nrcContext->BeginFrame(renderState.cmdList.Get(), frameSettings);
        renderState.nrcContext->PopulateShaderConstants(*paramBlockManager.nrcConstants);
    }
    else
    {
        memset(paramBlockManager.nrcConstants, 0, sizeof(NrcConstants));
    }

    const bool nrcDebugModeActive = (renderState.nrcContext != nullptr) &&
        (static_cast<NrcResolveMode>(SettingsManager::getAsUint("nrcResolveMode")) != NrcResolveMode::AddQueryResultToOutput);
    if (nrcDebugModeActive)
    {
        debugOutputTarget = &renderState.nrcDebugTarget;
        debugParams->debugOutputSrvIdx = renderState.nrcDebugTarget.getSrvIdx();
        debugParams->debugOutputNumChannels = renderState.nrcDebugTarget.debugOutputNumChannels;
    }

    auto& sceneParams = paramBlockManager.sceneParams;
    sceneParams->numAreaLights = renderState.scene.getNumAreaLights();
    sceneParams->cameraUnderwater = 0;
    sceneParams->voxelBoundsMin_WS = { 0, 0, 0 };
    sceneParams->voxelBoundsMax_WS = { 0, 0, 0 };
    sceneParams->biomeMapTexelsPerSide = 0;

    if (renderState.voxelMode)
    {
        const glm::ivec3 voxelBoundsMin_WS = Terrain::getVoxelRenderBoundsMin_WS();
        const glm::ivec3 voxelBoundsMax_WS = Terrain::getVoxelRenderBoundsMax_WS();
        const glm::ivec3 globalInstanceOffset = renderState.scene.getGlobalInstanceOffset();

        sceneParams->cameraUnderwater = Terrain::isCameraUnderwater() ? 1 : 0;

        const glm::ivec2 biomeMapOrigin = BiomeMap::getOriginBlocksXZ_WS();
        sceneParams->biomeMapOriginBlocksXZ_WS = { biomeMapOrigin.x, biomeMapOrigin.y /*z*/ };
        sceneParams->biomeMapTexelsPerSide = BiomeMap::getTexelsPerSide();
        paramBlockManager.heapIndices->srv.biomeMapIdx = BiomeMap::getSrvIdx();
        sceneParams->voxelBoundsMin_WS = {
            voxelBoundsMin_WS.x - globalInstanceOffset.x,
            voxelBoundsMin_WS.y - globalInstanceOffset.y,
            voxelBoundsMin_WS.z - globalInstanceOffset.z,
        };
        sceneParams->voxelBoundsMax_WS = {
            voxelBoundsMax_WS.x - globalInstanceOffset.x,
            voxelBoundsMax_WS.y - globalInstanceOffset.y,
            voxelBoundsMax_WS.z - globalInstanceOffset.z,
        };
    }

    ID3D12DescriptorHeap* const descHeaps[] = { renderState.sharedDescriptorHeap.Get() };

    // Light tree build runs whenever the topology flag is set, regardless of
    // the accumulate-stalled gate below. Otherwise an emitter change during an
    // accumulate-stalled window would clear the flag (top of Scene::update next
    // frame) without anyone consuming it, leaving the tree stale on resume.
    // SetDescriptorHeaps must precede any SetComputeRootSignature because the
    // root sigs declare CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED.
    if (renderState.scene.hasTlas())
    {
        renderState.cmdList->SetDescriptorHeaps(std::size(descHeaps), descHeaps);
        renderState.lightTreeManager.update(renderState.cmdList.Get(), frameCtx.toFreeList);
        renderState.lightTreeManager.transitionForPathTracingRead(renderState.cmdList.Get());
    }

    // rtslParams must be populated AFTER lightTreeManager.update — mortonCapacity is
    // first set inside ensureLightTreeCapacity, so on the build frame the pre-update
    // value is 0 and the path tracer's treeLeafCount==0 guard short-circuits RTSL.
    auto& rtslParams = paramBlockManager.rtslParams;
    {
        const uint32_t numAreaLights = sceneParams->numAreaLights;
        const uint32_t M = (numAreaLights == 0) ? 0u : renderState.lightTreeManager.getTreeLeafCapacity();
        rtslParams->treeLeafCount = M;
        rtslParams->treeLeafBase = (M == 0) ? 0u : (M - 1u);
    }

    if (renderState.scene.hasTlas() && (!renderState.stopAccumulating || antialiasingMode != AntialiasingMode::ACCUMULATE))
    {
        // ===================================
        // SKY ATMOSPHERE LUTS
        // ===================================

        if (renderState.voxelMode)
        {
            const float cameraY = paramBlockManager.cameraParams->pos_WS.y +
                static_cast<float>(paramBlockManager.cameraParams->globalInstanceOffset.y);
            SkyAtmosphere::dispatch(renderState.cmdList.Get(), animTimeFloat, cameraY);
        }

        // ===================================
        // GBUFFER
        // ===================================

        // this isn't strictly necessary as the RtTargets should be promoted to UNORDERED_ACCESS on first access,
        // but it helps with state tracking (since otherwise the transition to PIXEL_SHADER_RESOURCE would complain that
        // the before state doesn't match reality)
        {
            BufferHelper::TransitionBatch batch;
            for (RtTarget* rtTarget : renderState.autoTransitionRtTargets)
            {
                if (rtTarget->hasUav)
                {
                    rtTarget->addTransitionTo(batch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
            }
            batch.submit(renderState.cmdList.Get());
        }

        renderState.cmdList->SetPipelineState1(renderState.gbufferPso.Get());
        renderState.cmdList->SetComputeRootSignature(renderState.gbufferRootSig.Get());

        renderState.cmdList->SetComputeRootConstantBufferView(GBUFFER_PARAM_IDX(GLOBAL_PARAMS), paramBlockManager.getParamBufferGpuAddress());
        bindSceneSrvs(GBUFFER_PARAM_IDX(RAYTRACING_ACS));
        renderState.cmdList->SetComputeRootUnorderedAccessView(GBUFFER_PARAM_IDX(GBUFFER_OUT), renderState.dev_gbuffer->GetGPUVirtualAddress());

        const D3D12_RESOURCE_DESC& pathTracingTargetDesc = renderState.pathTracingTarget.getTarget()->GetDesc();
        renderState.gbufferDispatchDesc.Width = static_cast<uint32_t>(pathTracingTargetDesc.Width);
        renderState.gbufferDispatchDesc.Height = pathTracingTargetDesc.Height;
        renderState.cmdList->DispatchRays(&renderState.gbufferDispatchDesc);

        // dev_gbuffer should be promoted to UNORDERED_ACCESS when first accessed by the gbuffer, and then should
        // decay back to COMMON after executing the command list
        BufferHelper::stateTransitionResourceBarrier(renderState.cmdList.Get(),
                                                     renderState.dev_gbuffer.Get(),
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        dispatchPathTracing(paramBlockManager, doPathSplitting);

        // ===================================
        // COLLECT
        // ===================================

        {
            BufferHelper::TransitionBatch batch;
            batch.add(renderState.dev_pathTracingRawBuffer.Get(),
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            batch.add(renderState.dev_ptDiffuseAlbedoRawBuffer.Get(),
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            batch.submit(renderState.cmdList.Get());
        }

        renderState.cmdList->SetPipelineState(renderState.collectPso.Get());
        renderState.cmdList->SetComputeRootSignature(renderState.collectRootSig.Get());

        renderState.cmdList->SetComputeRootConstantBufferView(COLLECT_PARAM_IDX(GLOBAL_PARAMS), paramBlockManager.getParamBufferGpuAddress());
        renderState.cmdList->SetComputeRootShaderResourceView(COLLECT_PARAM_IDX(PATH_TRACING_RAW_BUFFER_IN), renderState.dev_pathTracingRawBuffer->GetGPUVirtualAddress());
        renderState.cmdList->SetComputeRootShaderResourceView(COLLECT_PARAM_IDX(PT_DIFFUSE_ALBEDO_RAW_BUFFER_IN), renderState.dev_ptDiffuseAlbedoRawBuffer->GetGPUVirtualAddress());

        const uint32_t ptWidth = renderState.gbufferDispatchDesc.Width * (doPathSplitting ? 2 : 1);
        const uint32_t ptHeight = renderState.gbufferDispatchDesc.Height;
        const uint32_t dispatchWidth = Util::calculateDispatchSize(ptWidth, COLLECT_WORKGROUP_SIZE_X);
        const uint32_t dispatchHeight = Util::calculateDispatchSize(ptHeight, COLLECT_WORKGROUP_SIZE_Y);
        renderState.cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

        {
            BufferHelper::TransitionBatch batch;
            batch.add(renderState.dev_pathTracingRawBuffer.Get(),
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            batch.add(renderState.dev_ptDiffuseAlbedoRawBuffer.Get(),
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            batch.submit(renderState.cmdList.Get());
        }

        // ===================================
        // DLSS
        // ===================================

        if (useDlss)
        {
            const sl::BaseStructure* inputs[] = { &renderState.dlss.viewportHandle };
            CHECK_SL_RESULT(
                slEvaluateFeature(sl::kFeatureDLSS_RR, *frameToken, inputs, _countof(inputs), renderState.cmdList.Get()));
        }
    }
    else
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(3)); // prevent insanely high frame rate if not doing any meaningful work
    }

    // ===================================
    // POSTPROCESSING
    // ===================================

    renderState.cmdList->SetDescriptorHeaps(std::size(descHeaps), descHeaps);

    ComPtr<ID3D12Resource> backBuffer;
    const uint32_t currentBackBufferIndex = renderState.proxySwapChain->GetCurrentBackBufferIndex();
    CHECK_HRESULT(renderState.proxySwapChain->GetBuffer(currentBackBufferIndex, IID_PPV_ARGS(&backBuffer)));

    {
        BufferHelper::TransitionBatch batch;
        batch.add(backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        for (RtTarget* rtTarget : renderState.autoTransitionRtTargets)
        {
            if (rtTarget->hasSrv)
            {
                rtTarget->addTransitionTo(batch, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
        batch.submit(renderState.cmdList.Get());
    }

    const bool isAnyDebugViewActive = (debugOutputTarget != nullptr);

    if (isAnyDebugViewActive)
    {
        renderState.cmdList->SetPipelineState(renderState.debugViewPso.Get());
        renderState.cmdList->SetGraphicsRootSignature(renderState.debugViewRootSig.Get());
        renderState.cmdList->SetGraphicsRootConstantBufferView(DEBUG_VIEW_PARAM_IDX(GLOBAL_PARAMS),
                                                   paramBlockManager.getParamBufferGpuAddress());
    }
    else
    {
        renderState.cmdList->SetPipelineState(renderState.postprocessPso.Get());
        renderState.cmdList->SetGraphicsRootSignature(renderState.postprocessRootSig.Get());
        renderState.cmdList->SetGraphicsRootConstantBufferView(POSTPROCESS_PARAM_IDX(GLOBAL_PARAMS),
                                                   paramBlockManager.getParamBufferGpuAddress());
    }

    renderState.cmdList->RSSetViewports(1, &renderState.viewport);
    renderState.cmdList->RSSetScissorRects(1, &renderState.scissor);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpuHandle = renderState.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvCpuHandle.ptr +=
        currentBackBufferIndex * renderState.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    renderState.cmdList->OMSetRenderTargets(1, &rtvCpuHandle, FALSE, nullptr);

    const float clearColor[] = { 1.f, 0.f, 1.f, 1.f };
    renderState.cmdList->ClearRenderTargetView(rtvCpuHandle, clearColor, 0, nullptr);

    renderState.cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    renderState.cmdList->DrawInstanced(3, 1, 0, 0);

    if (renderState.screenshotRequest.active)
    {
        captureQueuedScreenshot();
    }

    if (showGui)
    {
        imguiEndFrame(deltaTime);
    }

    BufferHelper::stateTransitionResourceBarrier(
        renderState.cmdList.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    const bool nrcFrameActive = renderState.nrcContext != nullptr;
    submitCmd();

    if (nrcFrameActive)
    {
        renderState.nrcContext->EndFrame(renderState.graphicsCmdQueue.Get());
    }

    frameCtx.fenceValue = renderState.fence.signal(renderState.graphicsCmdQueue.Get());

    UINT syncInterval;
    UINT presentFlags;
    if (renderState.useVsync)
    {
        syncInterval = 1;
        presentFlags = 0;
    }
    else
    {
        syncInterval = 0;
        const bool isFullscreen = SettingsManager::getAsBool("fullscreen");
        presentFlags = (renderState.allowTearing && isFullscreen) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    }

    CHECK_HRESULT(renderState.proxySwapChain->Present(syncInterval, presentFlags));

    ++renderState.frameNumber;
    renderState.frameCtxIdx = (renderState.frameCtxIdx + 1) % NUM_FRAMES_IN_FLIGHT;

    updateFps(deltaTime);

    // TEMP: benchmark instrumentation
    {
        static const int benchmarkFrames = SettingsManager::getAsInt("benchmarkFrames");
        static int quietFrames = 0;
        static int measuredFrames = 0;
        static double accumTime = 0.0;
        if (benchmarkFrames > 0)
        {
            if (!Terrain::wasFrameQuiet() || renderState.scene.hasPendingBlasBuilds())
            {
                quietFrames = 0;
            }
            else if (++quietFrames > 120) // warmup after terrain settles
            {
                accumTime += deltaTime;
                if (++measuredFrames >= benchmarkFrames)
                {
                    Logger::log("benchmark: %d frames, avg %.3f ms",
                                measuredFrames, accumTime / measuredFrames * 1000.0);
                    Renderer::destroy();
                    exit(0);
                }
            }
        }
    }

    if (renderState.screenshotRequest.active)
    {
        finalizeQueuedScreenshot(); // this calls flush()

        if (renderState.testMode)
        {
            Renderer::destroy();
            exit(0);
        }
    }
}

static void beginFrame()
{
    FrameContext& frame = renderState.frameCtxs[renderState.frameCtxIdx];

    if (renderState.useWaitableSwapChain)
    {
        WaitForSingleObjectEx(renderState.frameLatencyWaitable, 1000 /*ms*/, true);
    }
    renderState.fence.waitFor(frame.fenceValue);

    frame.toFreeList.freeAll();
    CHECK_HRESULT(frame.cmdAlloc->Reset());
    CHECK_HRESULT(renderState.cmdList->Reset(frame.cmdAlloc.Get(), nullptr));
}

static void submitCmd()
{
    CHECK_HRESULT(renderState.cmdList->Close());
    renderState.graphicsCmdQueue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(renderState.cmdList.GetAddressOf()));
}

void flush()
{
    renderState.fence.waitFor(renderState.fence.signal(renderState.graphicsCmdQueue.Get()));

    for (auto& frame : renderState.frameCtxs)
    {
        frame.fenceValue = 0;
        frame.toFreeList.freeAll();
    }
}

uint32_t getFrameIndex()
{
    return renderState.frameCtxIdx;
}

void destroy()
{
    if (renderState.device == nullptr)
    {
        return;
    }

    flush();

    CHECK_SL_RESULT(slShutdown());

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    Terrain::shutdown();

    renderState.gpuRadixSort.destroy();
    renderState.lightTreeManager.destroy();
    WaterDisplacer::destroy();
    SkyAtmosphere::destroy();
    BiomeMap::destroy();

    renderState.scene.reset();
    AcsHelper::reset();

    for (RtTarget* rtTarget : renderState.allRtTargets)
    {
        rtTarget->reset();
    }

    renderState.dev_gbuffer.Reset();
    renderState.dev_pathTracingRawBuffer.Reset();
    renderState.dev_ptDiffuseAlbedoRawBuffer.Reset();

    destroyNrc();

    renderState.screenshotRequest.readbackBuffer.Reset();

    renderState.gbufferPso.Reset();
    renderState.ptPso.Reset();
    renderState.collectPso.Reset();
    renderState.nrcResolvePso.Reset();
    renderState.nrcUpdatePso.Reset();
    renderState.nrcQueryPso.Reset();
    renderState.postprocessPso.Reset();
    renderState.debugViewPso.Reset();

    renderState.gbufferRootSig.Reset();
    renderState.ptRootSig.Reset();
    renderState.collectRootSig.Reset();
    renderState.nrcResolveRootSig.Reset();
    renderState.postprocessRootSig.Reset();
    renderState.debugViewRootSig.Reset();

    renderState.dev_gbufferShaderIds.Reset();
    renderState.dev_ptShaderIds.Reset();
    renderState.dev_nrcUpdateShaderIds.Reset();
    renderState.dev_nrcQueryShaderIds.Reset();

    renderState.proxySwapChain.Reset();
    renderState.swapChain.Reset();
    renderState.rtvHeap.Reset();
    renderState.sharedDescriptorHeap.Reset();

    for (FrameContext& frameCtx : renderState.frameCtxs)
    {
        frameCtx.cmdAlloc.Reset();
        frameCtx.paramBlockManager.reset();
    }

    renderState.cmdList.Reset();

    renderState.fence.reset();

    if (renderState.useWaitableSwapChain && renderState.frameLatencyWaitable)
    {
        CloseHandle(renderState.frameLatencyWaitable);
    }

    renderState.graphicsCmdQueue.Reset();

#if ENABLE_ASSERTS
    ComPtr<ID3D12DebugDevice> debugDevice;
    if (SUCCEEDED(renderState.device.As(&debugDevice)))
    {
        debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
    }
#endif

    renderState.proxyDevice.Reset();
    renderState.device.Reset();
}

ID3D12Device5* getDevice()
{
    return renderState.device.Get();
}

ID3D12CommandQueue* getGraphicsQueue()
{
    return renderState.graphicsCmdQueue.Get();
}

bool getUseOmms()
{
    return renderState.useOmms;
}

const Camera& getCamera()
{
    return renderState.camera;
}

void restoreCameraFromImport(glm::ivec3 posInt, glm::vec3 posFloat, float phi, float theta)
{
    renderState.camera.restoreFromImport(posInt, posFloat, phi, theta);
}

float getAnimTime()
{
    return static_cast<float>(renderState.animTime);
}

const Scene& getScene()
{
    return renderState.scene;
}

} // namespace Renderer
