// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include "rendering/camera.h"
#include "rendering/window_manager.h"
#include "rendering/buffer/acs_helper.h"
#include "rendering/buffer/buffer_helper.h"
#include "rendering/common/common_enums.h"
#include "scene/gltf_loader.h"
#include "scene/scene.h"
#include "terrain/terrain.h"
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

static uint32_t frameCtxIdx = 0;
static HANDLE frameLatencyWaitable;

static constexpr float defaultFovYDegrees = 35;

void init()
{
    testMode = (SettingsManager::getAsString("testOutput") != "");
    voxelMode = SettingsManager::getAsBool("voxelMode");

    initStreamline();

    initDevice();
    initDescriptorHeaps();

    initNvapi();

    for (uint32_t frameIdx = 0; frameIdx < NUM_FRAMES_IN_FLIGHT; ++frameIdx)
    {
        FrameContext& frameCtx = frameCtxs[frameIdx];
        frameCtx.paramBlockManager.init();
        frameCtx.paramBlockManager.setName(L"paramBlockManager " + std::to_wstring(frameIdx));

        frameCtx.paramBlockManager.sceneParams->voxelMode = voxelMode ? 1 : 0;
    }

    useWaitableSwapChain = SettingsManager::getAsBool("useWaitableSwapChain");

    initSwapChain();
    initRtTargets();
    initCommand();
    initConstantParams();

    camera.init(XMConvertToRadians(defaultFovYDegrees));

    AcsHelper::init();

    scene.init();

    initRootSignature();
    initPipeline();

    initImgui();

    const std::string& defaultScene = SettingsManager::getAsString("scene");
    if (voxelMode)
    {
        Terrain::init(&scene);
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

    if (!testMode)
    {
        SetForegroundWindow(hwnd);
    }
}

static bool dlssNeedsReset = false;

void loadScene(const std::string& filePathStr)
{
    flush();
    GltfLoader::loadGltf(filePathStr, scene);
    if (nrcContext != nullptr)
    {
        configureNrc();
    }
    dlssNeedsReset = true;
}


static ComPtr<ID3D12Resource> dev_gbuffer;
static ComPtr<ID3D12Resource> dev_pathTracingRawBuffer;
static ComPtr<ID3D12Resource> dev_ptDiffuseAlbedoRawBuffer;

static std::array<D3D12_CPU_DESCRIPTOR_HANDLE, NUM_FRAMES_IN_FLIGHT> rtvHeapCpuHandles;

static float mipBias = 0.f;

static sl::ViewportHandle slViewportHandle{ 1738 }; // TODO: does this need to be a meaningful number?
static sl::Extent slRenderExtent;
static sl::Extent slViewportExtent;

static const std::vector<sl::DLSSMode> dlssModes = {
    sl::DLSSMode::eDLAA,
    sl::DLSSMode::eMaxQuality,
    sl::DLSSMode::eBalanced,
    sl::DLSSMode::eMaxPerformance,
    sl::DLSSMode::eUltraPerformance,
};

static sl::DLSSDOptions dlssdOptions;

void resize()
{
    if (!swapChain)
    {
        return;
    }

    frameNumber = 0;

    RECT rect;
    GetClientRect(hwnd, &rect);
    const uint32_t viewportWidth = std::max<uint32_t>(rect.right - rect.left, 1);
    const uint32_t viewportHeight = std::max<uint32_t>(rect.bottom - rect.top, 1);

    viewport = { 0, 0, static_cast<float>(viewportWidth), static_cast<float>(viewportHeight) };
    scissor = { 0, 0, static_cast<long>(viewportWidth), static_cast<long>(viewportHeight) };

    const AntialiasingMode antialiasingMode =
        static_cast<AntialiasingMode>(SettingsManager::getAsUint("antialiasingMode"));
    if (antialiasingMode == AntialiasingMode::DLSS)
    {
        slViewportExtent = { 0, 0, viewportWidth, viewportHeight };

        sl::DLSSDOptimalSettings dlssdSettings;
        dlssdOptions.mode = (sl::DLSSMode)dlssModes[SettingsManager::getAsUint("dlssMode")];
        dlssdOptions.outputWidth = viewportWidth;
        dlssdOptions.outputHeight = viewportHeight;
        CHECK_SL_RESULT(slDLSSDGetOptimalSettings(dlssdOptions, dlssdSettings));

        renderWidth = dlssdSettings.optimalRenderWidth;
        renderHeight = dlssdSettings.optimalRenderHeight;
        mipBias = std::log2(static_cast<float>(renderWidth) / static_cast<float>(viewportWidth)) - 1.f;

        slRenderExtent = { 0, 0, renderWidth, renderHeight };

        dlssdOptions.dlaaPreset = sl::DLSSDPreset::ePresetD;
        dlssdOptions.qualityPreset = sl::DLSSDPreset::ePresetD;
        dlssdOptions.balancedPreset = sl::DLSSDPreset::ePresetD;
        dlssdOptions.performancePreset = sl::DLSSDPreset::ePresetD;
        dlssdOptions.ultraPerformancePreset = sl::DLSSDPreset::ePresetD;
        dlssdOptions.colorBuffersHDR = sl::Boolean::eTrue;
        dlssdOptions.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
        dlssdOptions.alphaUpscalingEnabled = sl::Boolean::eFalse;
        CHECK_SL_RESULT(slDLSSDSetOptions(slViewportHandle, dlssdOptions));

        dlssNeedsReset = true;
    }
    else
    {
        renderWidth = viewportWidth;
        renderHeight = viewportHeight;
        mipBias = 0.f;
    }

    flush();

    CHECK_HRESULT(swapChain->ResizeBuffers(0, viewportWidth, viewportHeight, DXGI_FORMAT_UNKNOWN, swapChainFlags));
    if (useWaitableSwapChain)
    {
        CHECK_HRESULT(swapChain->SetMaximumFrameLatency(2));
        if (frameLatencyWaitable)
        {
            CloseHandle(frameLatencyWaitable);
        }
        frameLatencyWaitable = swapChain->GetFrameLatencyWaitableObject();
    }

    dev_gbuffer.Reset();
    dev_gbuffer = BufferHelper::createBasicBuffer(renderWidth * renderHeight * sizeof(GbufferData),
                                                  &DEFAULT_HEAP,
                                                  { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    dev_gbuffer->SetName(L"dev_gbuffer");

    dev_pathTracingRawBuffer.Reset();
    const bool doPathSplitting = SettingsManager::getAsBool("doPathSplitting");
    const uint32_t pathTracingRawBufferSizeBytes =
        renderWidth * renderHeight * (doPathSplitting ? 2 : 1) * sizeof(float) * 4;
    dev_pathTracingRawBuffer = BufferHelper::createBasicBuffer(
        pathTracingRawBufferSizeBytes, &DEFAULT_HEAP, { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    dev_pathTracingRawBuffer->SetName(L"dev_pathTracingRawBuffer");

    dev_ptDiffuseAlbedoRawBuffer.Reset();
    dev_ptDiffuseAlbedoRawBuffer = BufferHelper::createBasicBuffer(
        pathTracingRawBufferSizeBytes, &DEFAULT_HEAP, { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    dev_ptDiffuseAlbedoRawBuffer->SetName(L"dev_ptDiffuseAlbedoRawBuffer");

    const uint32_t rtvIncrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (uint32_t frameIdx = 0; frameIdx < NUM_FRAMES_IN_FLIGHT; ++frameIdx)
    {
        ComPtr<ID3D12Resource> backBuffer;
        CHECK_HRESULT(swapChain->GetBuffer(frameIdx, IID_PPV_ARGS(&backBuffer)));
        D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle = rtvHeapCpuHandles[frameIdx];
        cpuHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += frameIdx * rtvIncrementSize;
        device->CreateRenderTargetView(backBuffer.Get(), nullptr, cpuHandle);
        const std::wstring backBufferName = L"backBuffer " + std::to_wstring(frameIdx);
        backBuffer->SetName(backBufferName.c_str());
    }

    for (RtTarget* rtTarget : allRtTargets)
    {
        rtTarget->reset();
        if (rtTarget->isFullSize)
        {
            rtTarget->setDimensions(viewportWidth, viewportHeight);
        }
        else
        {
            rtTarget->setDimensions(renderWidth, renderHeight);
        }
        rtTarget->init();
    }

    nrcDebugTarget.reset();
    nrcDebugTarget.setDimensions(renderWidth * (doPathSplitting ? 2 : 1), renderHeight);
    nrcDebugTarget.init();

    for (auto& frame : frameCtxs)
    {
        frame.paramBlockManager.heapIndices->uav = {
            .pathTracingTargetIdx = pathTracingTarget.getUavIdx(),
            .diffuseAlbedoTargetIdx = diffuseAlbedoTarget.getUavIdx(),
            .specularAlbedoTargetIdx = specularAlbedoTarget.getUavIdx(),
            .linearDepthTargetIdx = linearDepthTarget.getUavIdx(),

            .normalsAndRoughnessTargetIdx = normalsAndRoughnessTarget.getUavIdx(),
            .motionTargetIdx = motionTarget.getUavIdx(),
            .specularHitDistanceTargetIdx = specularHitDistanceTarget.getUavIdx(),
            .debugTargetIdx = debugTarget.getUavIdx(),
        };

        frame.paramBlockManager.heapIndices->srv = {
            .pathTracingTargetIdx = pathTracingTarget.getSrvIdx(),
            .diffuseAlbedoTargetIdx = diffuseAlbedoTarget.getSrvIdx(),
            .specularAlbedoTargetIdx = specularAlbedoTarget.getSrvIdx(),
            .linearDepthTargetIdx = linearDepthTarget.getSrvIdx(),

            .normalsAndRoughnessTargetIdx = normalsAndRoughnessTarget.getSrvIdx(),
            .motionTargetIdx = motionTarget.getSrvIdx(),
            .specularHitDistanceTargetIdx = specularHitDistanceTarget.getSrvIdx(),
            .dlssOutputTargetIdx = dlssOutputTarget.getSrvIdx(),

            .debugTargetIdx = debugTarget.getSrvIdx(),
        };
    }

    camera.setAspectRatio(static_cast<float>(renderWidth) / static_cast<float>(renderHeight));

    // DLSS programming guide says to use this as the jitter sequence length:
    // Total Phases = Base Phase Count * (Target Resolution / Render Resolution) ^ 2
    //
    // Streamline programming guide says there's no reason to limit the sequence length, so I'm using 64 for the "Base
    // Phase Count" instead of the default/recommended of 8.
    const float dlssScaleFactor = static_cast<float>(viewportWidth) / static_cast<float>(renderWidth);
    const uint32_t jitterHaltonSequenceLength =
        static_cast<uint32_t>(ceilf(64 * (dlssScaleFactor * dlssScaleFactor)));
    camera.setJitterHaltonSequenceLength(jitterHaltonSequenceLength);

    frameTimeBuffer.clear();

    if (nrcContext != nullptr)
    {
        // TODO: try to avoid fully destroying and recreating NRC on resize
        destroyNrc();
        initNrc();
    }
}

void queueResize()
{
    needsResize = true;
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
    cmdList->SetComputeRootShaderResourceView(baseIdx + 0, scene.getDevTlasAddress());
    cmdList->SetComputeRootShaderResourceView(baseIdx + 1, scene.getDevVertsBufferAddress());
    cmdList->SetComputeRootShaderResourceView(baseIdx + 2, scene.getDevIdxsBufferAddress());
    cmdList->SetComputeRootShaderResourceView(baseIdx + 3, scene.getDevInstanceDatasAddress());
    cmdList->SetComputeRootShaderResourceView(baseIdx + 4, scene.getDevMaterialsAddress());
    cmdList->SetComputeRootShaderResourceView(baseIdx + 5, scene.getDevPerTriDatasBufferAddress());
    cmdList->SetComputeRootShaderResourceView(baseIdx + 6, scene.getDevAreaLightsBufferAddress());
    cmdList->SetComputeRootShaderResourceView(baseIdx + 7, scene.getDevAreaLightSamplingStructureAddress());
}

static void bindNrcBuffers(const nrc::d3d12::Buffers* nrcBuffers, uint32_t baseIdx)
{
    cmdList->SetComputeRootUnorderedAccessView(baseIdx + 0, (*nrcBuffers)[nrc::BufferIdx::QueryPathInfo].resource->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(baseIdx + 1, (*nrcBuffers)[nrc::BufferIdx::TrainingPathInfo].resource->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(baseIdx + 2, (*nrcBuffers)[nrc::BufferIdx::TrainingPathVertices].resource->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(baseIdx + 3, (*nrcBuffers)[nrc::BufferIdx::QueryRadianceParams].resource->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(baseIdx + 4, (*nrcBuffers)[nrc::BufferIdx::Counter].resource->GetGPUVirtualAddress());
}

static void bindPtCommonParams(ParamBlockManager& paramBlockManager)
{
    cmdList->SetComputeRootConstantBufferView(PT_PARAM_IDX(GLOBAL_PARAMS), paramBlockManager.getParamBufferGpuAddress());
    bindSceneSrvs(PT_PARAM_IDX(RAYTRACING_ACS));
    cmdList->SetComputeRootShaderResourceView(PT_PARAM_IDX(GBUFFER_IN), dev_gbuffer->GetGPUVirtualAddress());
}

static void dispatchPathTracing(ParamBlockManager& paramBlockManager, bool doPathSplitting)
{
    // ===================================
    // NRC UPDATE
    // ===================================

    if (nrcContext != nullptr)
    {
        cmdList->SetPipelineState1(nrcUpdatePso.Get());
        cmdList->SetComputeRootSignature(ptRootSig.Get());

        bindPtCommonParams(paramBlockManager);
        cmdList->SetComputeRootConstantBufferView(PT_PARAM_IDX(NRC_CONSTANTS), paramBlockManager.getNrcConstantsGpuAddress());
        bindNrcBuffers(nrcContext->GetBuffers(), PT_PARAM_IDX(NRC_QUERY_PATH_INFO));

        nrcUpdateDispatchDesc.Width = paramBlockManager.nrcConstants->trainingDimensions.x;
        nrcUpdateDispatchDesc.Height = paramBlockManager.nrcConstants->trainingDimensions.y;
        cmdList->DispatchRays(&nrcUpdateDispatchDesc);

        BufferHelper::uavBarrier(cmdList.Get(), nullptr);
    }

    // ===================================
    // PATH TRACING (or NRC QUERY)
    // ===================================

    const bool useNrcQuery = (nrcContext != nullptr);
    cmdList->SetPipelineState1(useNrcQuery ? nrcQueryPso.Get() : ptPso.Get());
    cmdList->SetComputeRootSignature(ptRootSig.Get());

    bindPtCommonParams(paramBlockManager);

    cmdList->SetComputeRootUnorderedAccessView(PT_PARAM_IDX(PATH_TRACING_RAW_BUFFER_OUT), dev_pathTracingRawBuffer->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(PT_PARAM_IDX(PT_DIFFUSE_ALBEDO_RAW_BUFFER_OUT), dev_ptDiffuseAlbedoRawBuffer->GetGPUVirtualAddress());

    if (useNrcQuery)
    {
        cmdList->SetComputeRootConstantBufferView(PT_PARAM_IDX(NRC_CONSTANTS), paramBlockManager.getNrcConstantsGpuAddress());
        bindNrcBuffers(nrcContext->GetBuffers(), PT_PARAM_IDX(NRC_QUERY_PATH_INFO));
    }

    D3D12_DISPATCH_RAYS_DESC& activePtDispatchDesc = useNrcQuery ? nrcQueryDispatchDesc : ptDispatchDesc;
    activePtDispatchDesc.Width = gbufferDispatchDesc.Width * (doPathSplitting ? 2 : 1);
    activePtDispatchDesc.Height = gbufferDispatchDesc.Height;
    cmdList->DispatchRays(&activePtDispatchDesc);

    // ===================================
    // NRC RESOLVE
    // ===================================

    if (useNrcQuery)
    {
        BufferHelper::uavBarrier(cmdList.Get(), nullptr);

        nrcContext->QueryAndTrain(cmdList.Get(), nullptr);

        ID3D12DescriptorHeap* const descHeaps[] = { sharedDescriptorHeap.Get() };
        cmdList->SetDescriptorHeaps(std::size(descHeaps), descHeaps);
        BufferHelper::uavBarrier(cmdList.Get(), nullptr);

        const NrcResolveMode resolveMode = static_cast<NrcResolveMode>(SettingsManager::getAsUint("nrcResolveMode"));
        const bool useBuiltinResolve = (resolveMode != NrcResolveMode::AddQueryResultToOutput);

        if (useBuiltinResolve)
        {
            nrcDebugTarget.transitionToState(cmdList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            nrcContext->Resolve(cmdList.Get(), nrcDebugTarget.getTarget());
            cmdList->SetDescriptorHeaps(std::size(descHeaps), descHeaps);
        }
        else
        {
            const nrc::d3d12::Buffers* nrcBuffers = nrcContext->GetBuffers();
            cmdList->SetPipelineState(nrcResolvePso.Get());
            cmdList->SetComputeRootSignature(nrcResolveRootSig.Get());

            cmdList->SetComputeRootConstantBufferView(NRC_RESOLVE_PARAM_IDX(NRC_CONSTANTS), paramBlockManager.getNrcConstantsGpuAddress());
            cmdList->SetComputeRootUnorderedAccessView(
                NRC_RESOLVE_PARAM_IDX(QUERY_PATH_INFO),
                (*nrcBuffers)[nrc::BufferIdx::QueryPathInfo].resource->GetGPUVirtualAddress());
            cmdList->SetComputeRootUnorderedAccessView(
                NRC_RESOLVE_PARAM_IDX(QUERY_RADIANCE),
                (*nrcBuffers)[nrc::BufferIdx::QueryRadiance].resource->GetGPUVirtualAddress());
            cmdList->SetComputeRootUnorderedAccessView(
                NRC_RESOLVE_PARAM_IDX(PATH_TRACING_RAW_BUFFER_OUT),
                dev_pathTracingRawBuffer->GetGPUVirtualAddress());

            const uint32_t nrcResolveDispatchWidth =
                Util::calculateDispatchSize(paramBlockManager.nrcConstants->frameDimensions.x, NRC_RESOLVE_WORKGROUP_SIZE_X);
            const uint32_t nrcResolveDispatchHeight =
                Util::calculateDispatchSize(paramBlockManager.nrcConstants->frameDimensions.y, NRC_RESOLVE_WORKGROUP_SIZE_Y);
            cmdList->Dispatch(nrcResolveDispatchWidth, nrcResolveDispatchHeight, 1);
        }

        BufferHelper::uavBarrier(cmdList.Get(), nullptr);
    }
}

static void beginFrame();
static void submitCmd();

static auto lastTimePoint = std::chrono::high_resolution_clock::now();
static bool stopAccumulating = false;

void render()
{
    if (needsResize)
    {
        resize();
        needsResize = false;
    }

    const bool showGui = SettingsManager::getAsBool("showGui");
    if (showGui)
    {
        imguiBeginFrame();
    }

    const auto currentTimePoint = std::chrono::high_resolution_clock::now();
    const double deltaTime = std::chrono::duration<double>(currentTimePoint - lastTimePoint).count();
    lastTimePoint = currentTimePoint;

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
            sl::Resource pathTracingResource = makeSlResource(&pathTracingTarget);
            sl::Resource dlssOutputResource = makeSlResource(&dlssOutputTarget);
            sl::Resource linearDepthResource = makeSlResource(&linearDepthTarget);
            sl::Resource motionResource = makeSlResource(&motionTarget);
            sl::Resource diffuseAlbedoResource = makeSlResource(&diffuseAlbedoTarget);
            sl::Resource specularAlbedoResource = makeSlResource(&specularAlbedoTarget);
            sl::Resource normalsAndRoughnessResource = makeSlResource(&normalsAndRoughnessTarget);
            sl::Resource specularHitDistanceResource = makeSlResource(&specularHitDistanceTarget);

            sl::ResourceTag resourceTags[] = {
                {&pathTracingResource, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &slRenderExtent},
                {&dlssOutputResource, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &slViewportExtent},
                {&linearDepthResource, sl::kBufferTypeLinearDepth, sl::ResourceLifecycle::eValidUntilPresent, &slRenderExtent},
                {&motionResource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &slRenderExtent},
                {&diffuseAlbedoResource, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &slRenderExtent},
                {&specularAlbedoResource, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &slRenderExtent},
                {&normalsAndRoughnessResource, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilPresent, &slRenderExtent},
                {&specularHitDistanceResource, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eValidUntilPresent, &slRenderExtent},
            };
            // clang-format on

            CHECK_SL_RESULT(
                slSetTagForFrame(*frameToken, slViewportHandle, resourceTags, _countof(resourceTags), cmdList.Get()));
        }

        slConstants = {};
        slConstants.depthInverted = sl::Boolean::eFalse;
        slConstants.cameraMotionIncluded = sl::Boolean::eTrue;
        slConstants.motionVectors3D = sl::Boolean::eFalse;
        slConstants.orthographicProjection = sl::Boolean::eFalse;
        slConstants.motionVectorsDilated = sl::Boolean::eFalse;
        slConstants.motionVectorsJittered = sl::Boolean::eFalse;

        if (dlssNeedsReset)
        {
            slConstants.reset = sl::Boolean::eTrue;
            dlssNeedsReset = false;
        }
        else
        {
            slConstants.reset = sl::Boolean::eFalse;
        }
    }

    auto& frameCtx = frameCtxs[frameCtxIdx];

    ParamBlockManager& paramBlockManager = frameCtx.paramBlockManager;

    PlayerInput playerInput = {};
    if (!SettingsManager::getAsBool("lockCamera"))
    {
        playerInput = WindowManager::getPlayerInput();
    }
    camera.processInput(deltaTime, playerInput);

    if (voxelMode)
    {
        Terrain::update(frameCtx.toFreeList);
    }

    const bool didSceneChange = scene.update(cmdList.Get(), frameCtx.toFreeList);

    const bool didCameraChange = camera.update();

    if (useDlss)
    {
        camera.copySlConstantsTo(&slConstants);
        CHECK_SL_RESULT(slSetConstants(slConstants, *frameToken, slViewportHandle));

        camera.copyMatricesToDlssOptions(&dlssdOptions.worldToCameraView, &dlssdOptions.cameraViewToWorld);
        CHECK_SL_RESULT(slDLSSDSetOptions(slViewportHandle, dlssdOptions));
    }

    camera.copyParamsTo(paramBlockManager.cameraParams);

    const bool resetAccumulation = didCameraChange || didSceneChange || didPathTracingSettingsChange;

    auto& renderParams = paramBlockManager.renderParams;
    renderParams->frameNumber = frameNumber;

    if (resetAccumulation)
    {
        accumulatedFrameNumber = 0;
        stopAccumulating = false;
    }
    else if (!stopAccumulating)
    {
        if (++accumulatedFrameNumber == SettingsManager::getAsUint("maxAccumulatedFrames"))
        {
            stopAccumulating = true;

            if (testMode)
            {
                queueScreenshot(true /*useTestOutputPath*/);
            }
        }
    }

    renderParams->accumulatedFrameNumber = accumulatedFrameNumber;
    renderParams->maxPathDepth = SettingsManager::getAsUint("maxPathDepth");
    renderParams->samplingMode = SettingsManager::getAsUint("samplingMode");
    renderParams->tonemapping = SettingsManager::getAsUint("tonemapping");
    renderParams->preTonemappedColorSrvIdx = useDlss ? dlssOutputTarget.getSrvIdx() : pathTracingTarget.getSrvIdx();
    renderParams->renderSize = { renderWidth, renderHeight };
    const bool doPathSplitting = SettingsManager::getAsBool("doPathSplitting");
    renderParams->doPathSplitting = doPathSplitting ? 1 : 0;
    renderParams->antialiasingMode = static_cast<uint32_t>(antialiasingMode);
    renderParams->refractionIndirectPassthrough = SettingsManager::getAsBool("refractionIndirectPassthrough") ? 1 : 0;
    renderParams->mipBias = mipBias;

    RtTarget* debugOutputTarget = nullptr;
    const std::string& debugViewSettingStr = SettingsManager::getAsString("debugView");
    if (debugViewComboMap.contains(debugViewSettingStr))
    {
        debugOutputTarget = debugViewComboMap.at(debugViewSettingStr);
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

    if (voxelMode)
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
    if (nrcContext != nullptr && maxPathDepth != nrcPrevMaxPathDepth)
    {
        configureNrc();
    }
    nrcPrevMaxPathDepth = maxPathDepth;

    if (nrcContext != nullptr)
    {
        nrc::FrameSettings frameSettings;
        frameSettings.maxExpectedAverageRadianceValue = SettingsManager::getAsFloat("nrcMaxRadiance");
        frameSettings.terminationHeuristicThreshold = SettingsManager::getAsFloat("nrcTerminationThreshold");
        frameSettings.trainingTerminationHeuristicThreshold = SettingsManager::getAsFloat("nrcTrainingTerminationThreshold");
        frameSettings.resolveMode = static_cast<NrcResolveMode>(SettingsManager::getAsUint("nrcResolveMode"));
        frameSettings.skipDeltaVertices = SettingsManager::getAsBool("nrcSkipDeltaVertices");
        frameSettings.trainTheCache = SettingsManager::getAsBool("nrcTrainTheCache");
        frameSettings.learningRate = SettingsManager::getAsFloat("nrcLearningRate");
        nrcContext->BeginFrame(cmdList.Get(), frameSettings);
        nrcContext->PopulateShaderConstants(*paramBlockManager.nrcConstants);
    }
    else
    {
        memset(paramBlockManager.nrcConstants, 0, sizeof(NrcConstants));
    }

    const bool nrcDebugModeActive = (nrcContext != nullptr) &&
        (static_cast<NrcResolveMode>(SettingsManager::getAsUint("nrcResolveMode")) != NrcResolveMode::AddQueryResultToOutput);
    if (nrcDebugModeActive)
    {
        debugOutputTarget = &nrcDebugTarget;
        debugParams->debugOutputSrvIdx = nrcDebugTarget.getSrvIdx();
        debugParams->debugOutputNumChannels = nrcDebugTarget.debugOutputNumChannels;
    }

    auto& sceneParams = paramBlockManager.sceneParams;
    sceneParams->numAreaLights = scene.getNumAreaLights();
    sceneParams->cameraUnderwater = 0;
    sceneParams->voxelBoundsMin_WS = { 0, 0, 0 };
    sceneParams->voxelBoundsMax_WS = { 0, 0, 0 };
    if (voxelMode)
    {
        const glm::ivec3 voxelBoundsMin_WS = Terrain::getVoxelRenderBoundsMin_WS();
        const glm::ivec3 voxelBoundsMax_WS = Terrain::getVoxelRenderBoundsMax_WS();
        const glm::ivec3 globalInstanceOffset = scene.getGlobalInstanceOffset();

        sceneParams->cameraUnderwater = Terrain::isCameraUnderwater() ? 1 : 0;
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

    ID3D12DescriptorHeap* const descHeaps[] = { sharedDescriptorHeap.Get() };

    if (scene.hasTlas() && (!stopAccumulating || antialiasingMode != AntialiasingMode::ACCUMULATE))
    {
        cmdList->SetDescriptorHeaps(std::size(descHeaps), descHeaps);

        // ===================================
        // GBUFFER
        // ===================================

        // this isn't strictly necessary as the RtTargets should be promoted to UNORDERED_ACCESS on first access,
        // but it helps with state tracking (since otherwise the transition to PIXEL_SHADER_RESOURCE would complain that
        // the before state doesn't match reality)
        {
            BufferHelper::TransitionBatch batch;
            for (RtTarget* rtTarget : autoTransitionRtTargets)
            {
                if (rtTarget->hasUav)
                {
                    rtTarget->addTransitionTo(batch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
            }
            batch.submit(cmdList.Get());
        }

        cmdList->SetPipelineState1(gbufferPso.Get());
        cmdList->SetComputeRootSignature(gbufferRootSig.Get());

        cmdList->SetComputeRootConstantBufferView(GBUFFER_PARAM_IDX(GLOBAL_PARAMS), paramBlockManager.getParamBufferGpuAddress());
        bindSceneSrvs(GBUFFER_PARAM_IDX(RAYTRACING_ACS));
        cmdList->SetComputeRootUnorderedAccessView(GBUFFER_PARAM_IDX(GBUFFER_OUT), dev_gbuffer->GetGPUVirtualAddress());

        const D3D12_RESOURCE_DESC& pathTracingTargetDesc = pathTracingTarget.getTarget()->GetDesc();
        gbufferDispatchDesc.Width = static_cast<uint32_t>(pathTracingTargetDesc.Width);
        gbufferDispatchDesc.Height = pathTracingTargetDesc.Height;
        cmdList->DispatchRays(&gbufferDispatchDesc);

        // dev_gbuffer should be promoted to UNORDERED_ACCESS when first accessed by the gbuffer, and then should
        // decay back to COMMON after executing the command list
        BufferHelper::stateTransitionResourceBarrier(cmdList.Get(),
                                                     dev_gbuffer.Get(),
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        dispatchPathTracing(paramBlockManager, doPathSplitting);

        // ===================================
        // COLLECT
        // ===================================

        {
            BufferHelper::TransitionBatch batch;
            batch.add(dev_pathTracingRawBuffer.Get(),
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            batch.add(dev_ptDiffuseAlbedoRawBuffer.Get(),
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            batch.submit(cmdList.Get());
        }

        cmdList->SetPipelineState(collectPso.Get());
        cmdList->SetComputeRootSignature(collectRootSig.Get());

        cmdList->SetComputeRootConstantBufferView(COLLECT_PARAM_IDX(GLOBAL_PARAMS), paramBlockManager.getParamBufferGpuAddress());
        cmdList->SetComputeRootShaderResourceView(COLLECT_PARAM_IDX(PATH_TRACING_RAW_BUFFER_IN), dev_pathTracingRawBuffer->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(COLLECT_PARAM_IDX(PT_DIFFUSE_ALBEDO_RAW_BUFFER_IN), dev_ptDiffuseAlbedoRawBuffer->GetGPUVirtualAddress());

        const uint32_t ptWidth = gbufferDispatchDesc.Width * (doPathSplitting ? 2 : 1);
        const uint32_t ptHeight = gbufferDispatchDesc.Height;
        const uint32_t dispatchWidth = Util::calculateDispatchSize(ptWidth, COLLECT_WORKGROUP_SIZE_X);
        const uint32_t dispatchHeight = Util::calculateDispatchSize(ptHeight, COLLECT_WORKGROUP_SIZE_Y);
        cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

        {
            BufferHelper::TransitionBatch batch;
            batch.add(dev_pathTracingRawBuffer.Get(),
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            batch.add(dev_ptDiffuseAlbedoRawBuffer.Get(),
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            batch.submit(cmdList.Get());
        }

        // ===================================
        // DLSS
        // ===================================

        if (useDlss)
        {
            const sl::BaseStructure* inputs[] = { &slViewportHandle };
            CHECK_SL_RESULT(
                slEvaluateFeature(sl::kFeatureDLSS_RR, *frameToken, inputs, _countof(inputs), cmdList.Get()));
        }
    }
    else
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(3)); // prevent insanely high frame rate if not doing any meaningful work
    }

    // ===================================
    // POSTPROCESSING
    // ===================================

    cmdList->SetDescriptorHeaps(std::size(descHeaps), descHeaps);

    ComPtr<ID3D12Resource> backBuffer;
    const uint32_t currentBackBufferIndex = swapChain->GetCurrentBackBufferIndex();
    CHECK_HRESULT(swapChain->GetBuffer(currentBackBufferIndex, IID_PPV_ARGS(&backBuffer)));

    {
        BufferHelper::TransitionBatch batch;
        batch.add(backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        for (RtTarget* rtTarget : autoTransitionRtTargets)
        {
            if (rtTarget->hasSrv)
            {
                rtTarget->addTransitionTo(batch, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
        batch.submit(cmdList.Get());
    }

    const bool isAnyDebugViewActive = (debugOutputTarget != nullptr);

    if (isAnyDebugViewActive)
    {
        cmdList->SetPipelineState(debugViewPso.Get());
        cmdList->SetGraphicsRootSignature(debugViewRootSig.Get());
        cmdList->SetGraphicsRootConstantBufferView(DEBUG_VIEW_PARAM_IDX(GLOBAL_PARAMS),
                                                   paramBlockManager.getParamBufferGpuAddress());
    }
    else
    {
        cmdList->SetPipelineState(postprocessPso.Get());
        cmdList->SetGraphicsRootSignature(postprocessRootSig.Get());
        cmdList->SetGraphicsRootConstantBufferView(POSTPROCESS_PARAM_IDX(GLOBAL_PARAMS),
                                                   paramBlockManager.getParamBufferGpuAddress());
    }

    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpuHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvCpuHandle.ptr +=
        currentBackBufferIndex * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    cmdList->OMSetRenderTargets(1, &rtvCpuHandle, FALSE, nullptr);

    const float clearColor[] = { 1.f, 0.f, 1.f, 1.f };
    cmdList->ClearRenderTargetView(rtvCpuHandle, clearColor, 0, nullptr);

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);

    if (screenshotRequest.active)
    {
        captureQueuedScreenshot();
    }

    if (showGui)
    {
        imguiEndFrame(deltaTime);
    }

    BufferHelper::stateTransitionResourceBarrier(
        cmdList.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    const bool nrcFrameActive = nrcContext != nullptr;
    submitCmd();

    if (nrcFrameActive)
    {
        nrcContext->EndFrame(graphicsCmdQueue.Get());
    }

    frameCtx.fenceValue = fence.signal(graphicsCmdQueue.Get());

    UINT syncInterval;
    UINT presentFlags;
    if (useVsync)
    {
        syncInterval = 1;
        presentFlags = 0;
    }
    else
    {
        syncInterval = 0;
        const bool isFullscreen = SettingsManager::getAsBool("fullscreen");
        presentFlags = (allowTearing && isFullscreen) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    }

    CHECK_HRESULT(swapChain->Present(syncInterval, presentFlags));

    ++frameNumber;
    frameCtxIdx = (frameCtxIdx + 1) % NUM_FRAMES_IN_FLIGHT;

    updateFps(deltaTime);

    if (screenshotRequest.active)
    {
        finalizeQueuedScreenshot(); // this calls flush()

        if (testMode)
        {
            Renderer::destroy();
            exit(0);
        }
    }
}

static void beginFrame()
{
    FrameContext& frame = frameCtxs[frameCtxIdx];

    if (useWaitableSwapChain)
    {
        WaitForSingleObjectEx(frameLatencyWaitable, 1000 /*ms*/, true);
    }
    fence.waitFor(frame.fenceValue);

    frame.toFreeList.freeAll();
    CHECK_HRESULT(frame.cmdAlloc->Reset());
    CHECK_HRESULT(cmdList->Reset(frame.cmdAlloc.Get(), nullptr));
}

static void submitCmd()
{
    CHECK_HRESULT(cmdList->Close());
    graphicsCmdQueue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(cmdList.GetAddressOf()));
}

void flush()
{
    fence.waitFor(fence.signal(graphicsCmdQueue.Get()));

    for (auto& frame : frameCtxs)
    {
        frame.fenceValue = 0;
        frame.toFreeList.freeAll();
    }
}

uint32_t getFrameIndex()
{
    return frameCtxIdx;
}

void destroy()
{
    if (device == nullptr)
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

    scene.reset();
    AcsHelper::reset();

    for (RtTarget* rtTarget : allRtTargets)
    {
        rtTarget->reset();
    }

    dev_gbuffer.Reset();
    dev_pathTracingRawBuffer.Reset();
    dev_ptDiffuseAlbedoRawBuffer.Reset();

    destroyNrc();

    screenshotRequest.readbackBuffer.Reset();

    gbufferPso.Reset();
    ptPso.Reset();
    collectPso.Reset();
    nrcResolvePso.Reset();
    nrcUpdatePso.Reset();
    nrcQueryPso.Reset();
    postprocessPso.Reset();
    debugViewPso.Reset();

    gbufferRootSig.Reset();
    ptRootSig.Reset();
    collectRootSig.Reset();
    nrcResolveRootSig.Reset();
    postprocessRootSig.Reset();
    debugViewRootSig.Reset();

    dev_gbufferShaderIds.Reset();
    dev_ptShaderIds.Reset();
    dev_nrcUpdateShaderIds.Reset();
    dev_nrcQueryShaderIds.Reset();

    swapChain.Reset();
    rtvHeap.Reset();
    sharedDescriptorHeap.Reset();

    for (FrameContext& frameCtx : frameCtxs)
    {
        frameCtx.cmdAlloc.Reset();
        frameCtx.paramBlockManager.reset();
    }

    cmdList.Reset();

    fence.reset();

    if (useWaitableSwapChain && frameLatencyWaitable)
    {
        CloseHandle(frameLatencyWaitable);
    }

    graphicsCmdQueue.Reset();
    factory.Reset();

#if ENABLE_ASSERTS
    ComPtr<ID3D12DebugDevice> debugDevice;
    if (SUCCEEDED(device.As(&debugDevice)))
    {
        debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
    }
#endif

    device.Reset();
}

ID3D12Device5* getDevice()
{
    return device.Get();
}

ID3D12CommandQueue* getGraphicsQueue()
{
    return graphicsCmdQueue.Get();
}

const Camera& getCamera()
{
    return camera;
}

const Scene& getScene()
{
    return scene;
}

} // namespace Renderer
