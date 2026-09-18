// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include <cstdlib>
#include <future>
#include <random>

#include "gpu_requirements.h"

#include <sl_security.h>

#include <nvShaderExtnEnums.h>
#include <nvapi.h>
#undef min
#undef max

#include "logger.h"
#include "rendering/buffer/buffer_helper.h"
#include "rendering/window_manager.h"
#include "settings_manager.h"
#include "util/util.h"

using WindowManager::hwnd;

namespace Renderer
{

// slInit mostly loads plugin DLLs and needs no device, so it runs alongside native device
// creation; initDevice joins it before handing the device to Streamline
static std::future<sl::Result> slInitResult;

// DLSS-G cannot run without Reflex, and Reflex in turn requires PCL for its latency markers
static constexpr sl::Feature slFeatures[] = { sl::kFeatureDLSS_RR, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };

void initStreamline()
{
    const std::wstring targetFileDirPath = Util::to_wstring(TARGET_FILE_DIR);
    const std::wstring slInterposerDllPath = targetFileDirPath + L"/sl.interposer.dll";

    // TODO: verify using WinVerifyTrust

    if (!sl::security::verifyEmbeddedSignature(slInterposerDllPath.c_str()))
    {
        Logger::logError("Could not verify signature of sl.interposer.dll");
        exit(1);
    }

    sl::Preferences prefs = {};
    prefs.showConsole = false;
    prefs.logLevel = renderState.headless ? sl::LogLevel::eOff : sl::LogLevel::eDefault;

    if (SettingsManager::getAsBool("verboseLogging"))
    {
        prefs.showConsole = true;
        prefs.logLevel = sl::LogLevel::eVerbose;
    }

    prefs.featuresToLoad = slFeatures;
    prefs.numFeaturesToLoad = _countof(slFeatures);

    prefs.applicationId = 1738; // TODO: not sure what to put here lol

    prefs.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    prefs.flags |= sl::PreferenceFlags::eUseManualHooking;
    // The over-the-air update check is a network round trip inside slInit
    prefs.flags &= ~(sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins);

    slInitResult = std::async(std::launch::async, [prefs]() { return slInit(prefs); });
}

namespace
{
// Frame generation is optional, so a missing feature only disables it rather than failing startup
void initFrameGenSupport(const sl::AdapterInfo& adapterInfo)
{
    // Generated frames would corrupt golden screenshots, and Reflex pacing the frame start would
    // skew perf measurements, so a headless run stays on the pre-frame-generation code path
    if (renderState.headless)
    {
        return;
    }

    const std::pair<sl::Feature, const char*> requiredFeatures[] = {
        { sl::kFeatureDLSS_G, "DLSS-G" },
        { sl::kFeatureReflex, "Reflex" },
        { sl::kFeaturePCL, "PCL" },
    };

    for (const auto& [feature, name] : requiredFeatures)
    {
        if (SL_FAILED(result, slIsFeatureSupported(feature, adapterInfo)))
        {
            renderState.frameGen.unsupportedReason = slResultToString(result);
            Logger::logWarning("%s not supported: %s", name, renderState.frameGen.unsupportedReason.c_str());
            return;
        }
    }

    // Reflex is driven entirely by slReflexSleep and the PCL markers in render(); the mode only
    // needs setting once, but it must be set even when frame generation is never switched on
    sl::ReflexOptions reflexOptions{};
    reflexOptions.mode = sl::ReflexMode::eLowLatency;
    CHECK_SL_RESULT(slReflexSetOptions(reflexOptions));

    sl::PCLState pclState{};
    CHECK_SL_RESULT(slPCLGetState(pclState));
    renderState.frameGen.pclStatsWindowMessage = pclState.statsWindowMessage;

    Logger::log("Frame generation supported");
    renderState.frameGen.supported = true;
}

// Loading the plugin is what makes SL hand back a proxy swap chain that renders off-screen.
// Unloading it again when frame generation is off is the only way to avoid the resulting extra
// copy and cross-queue sync, which is why the swap chain has to be (re)created after every call.
void loadFrameGenPlugin(bool active)
{
    CHECK_SL_RESULT(slSetFeatureLoaded(sl::kFeatureDLSS_G, active));
    renderState.frameGen.active = active;
    if (!active)
    {
        renderState.frameGen.framesPresentedLastFrame = 1;
    }
}

[[noreturn]] void failGpuCompatibility(const std::string& reason)
{
    const std::string message = reason + "\nUpdate your graphics driver and try again. If this persists, use a GPU and "
                                         "driver that support these features.";
    Logger::logError("%s", message.c_str());
    if (!renderState.headless)
    {
        const std::wstring wideMessage = Util::to_wstring(message.c_str());
        MessageBoxW(hwnd, wideMessage.c_str(), L"Biomeinator - unsupported GPU capabilities", MB_OK | MB_ICONERROR);
    }
    std::exit(EXIT_FAILURE);
}

std::string shaderModelName(D3D_SHADER_MODEL model)
{
    const unsigned int value = static_cast<unsigned int>(model);
    return std::to_string(value >> 4) + "." + std::to_string(value & 0xf);
}

void checkRaytracingSupport(const std::string& adapterName)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    if (FAILED(renderState.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
    {
        failGpuCompatibility("GPU '" + adapterName + "': unable to query DirectX raytracing support.");
    }
    if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
    {
        failGpuCompatibility("GPU '" + adapterName +
                             "' does not support the required DirectX Raytracing tier 1.1 (inline raytracing).");
    }
    renderState.useOmms = renderState.voxelMode && options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_2;
    if (renderState.voxelMode)
    {
        if (renderState.useOmms)
        {
            Logger::log("Raytracing tier 1.2 supported, using opacity micromaps");
        }
        else
        {
            Logger::logWarning("Raytracing tier 1.2 not supported, disabling opacity micromaps");
        }
    }
}
} // namespace

void initDevice()
{
    // sl.interposer exports the same entry points and is linked first, so the native ones are
    // fetched from the system DLLs by hand
    typedef HRESULT(WINAPI * PFunD3D12GetDebugInterface)(REFIID, void**);
    typedef HRESULT(WINAPI * PFunCreateDXGIFactory2)(UINT, REFIID, void**);
    typedef HRESULT(WINAPI * PFunD3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    const HMODULE d3d12Mod = LoadLibrary("d3d12.dll");
    const HMODULE dxgiMod = LoadLibrary("dxgi.dll");
    const auto d3d12GetDebugInterface =
        reinterpret_cast<PFunD3D12GetDebugInterface>(GetProcAddress(d3d12Mod, "D3D12GetDebugInterface"));
    const auto createDxgiFactory2 =
        reinterpret_cast<PFunCreateDXGIFactory2>(GetProcAddress(dxgiMod, "CreateDXGIFactory2"));
    const auto d3d12CreateDevice =
        reinterpret_cast<PFunD3D12CreateDevice>(GetProcAddress(d3d12Mod, "D3D12CreateDevice"));

    UINT dxgiFactoryFlags = 0;

    if (SettingsManager::getAsBool("gpuValidation"))
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(d3d12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            Logger::log("Enabled debug layer");
            debug->EnableDebugLayer();

            ComPtr<ID3D12Debug1> debug1;
            if (SUCCEEDED(debug.As(&debug1)))
            {
                debug1->SetEnableGPUBasedValidation(true);
            }

            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
        else
        {
            Logger::logError("Failed to enable debug layer");
            exit(1);
        }
    }

    CHECK_HRESULT(createDxgiFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&renderState.factory)));

    DXGI_ADAPTER_DESC1 adapterDesc;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; renderState.factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        CHECK_HRESULT(adapter->GetDesc1(&adapterDesc));
        if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            adapter.Reset();
            continue;
        }

        if (SUCCEEDED(d3d12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&renderState.device))))
        {
            constexpr auto requiredShaderModel = static_cast<D3D_SHADER_MODEL>(BIOMEINATOR_REQUIRED_SHADER_MODEL);
            D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{ requiredShaderModel };
            const HRESULT shaderModelResult = renderState.device->CheckFeatureSupport(
                D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel));
            renderState.adapterName = Util::to_string(adapterDesc.Description);
            if (FAILED(shaderModelResult))
            {
                failGpuCompatibility("GPU '" + renderState.adapterName + "': unable to query required Shader Model " +
                                     shaderModelName(requiredShaderModel) + " support.");
            }
            if (shaderModel.HighestShaderModel < requiredShaderModel)
            {
                failGpuCompatibility("GPU '" + renderState.adapterName + "' reports Shader Model " +
                                     shaderModelName(shaderModel.HighestShaderModel) + "; this build requires Shader Model " +
                                     shaderModelName(requiredShaderModel) + ".");
            }
            Logger::log("Shader Model %s requirement satisfied", shaderModelName(requiredShaderModel).c_str());
            checkRaytracingSupport(renderState.adapterName);
            Logger::log("Selected adapter: %ls", adapterDesc.Description);
            break;
        }

        adapter.Reset();
    }

    if (!renderState.device)
    {
        failGpuCompatibility("No hardware adapter supporting Direct3D feature level 12.1 was found.");
    }

    // The RT pipelines only need the native device, so their creation overlaps with the
    // Streamline setup below and the rest of init
    startRtPipelineCreation();

    timedInitStep("slInit", []() { CHECK_SL_RESULT(slInitResult.get()); });

    // Manual hooking: the native interfaces are wrapped in Streamline proxies after the fact.
    // Only the calls Streamline hooks (queue and swap chain creation, present) go through the
    // proxies; everything else uses the native interfaces.
    {
        ID3D12Device* proxyDevice = renderState.device.Get();
        CHECK_SL_RESULT(slUpgradeInterface(reinterpret_cast<void**>(&proxyDevice)));
        CHECK_HRESULT(proxyDevice->QueryInterface(IID_PPV_ARGS(&renderState.proxyDevice)));

        IDXGIFactory* proxyFactory = renderState.factory.Get();
        CHECK_SL_RESULT(slUpgradeInterface(reinterpret_cast<void**>(&proxyFactory)));
        CHECK_HRESULT(proxyFactory->QueryInterface(IID_PPV_ARGS(&renderState.proxyFactory)));
    }

    timedInitStep("slSetD3DDevice", []() { CHECK_SL_RESULT(slSetD3DDevice(renderState.device.Get())); });

    sl::AdapterInfo adapterInfo{};
    adapterInfo.deviceLUID = (uint8_t*)&adapterDesc.AdapterLuid;
    adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

    CHECK_SL_RESULT(slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo));

    initFrameGenSupport(adapterInfo);

    D3D12_COMMAND_QUEUE_DESC graphicsCmdQueueDesc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
    };
    CHECK_HRESULT(renderState.proxyDevice->CreateCommandQueue(&graphicsCmdQueueDesc, IID_PPV_ARGS(&renderState.graphicsCmdQueue)));

    renderState.fence.init();
}

void initDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC sharedHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = SHARED_DESCRIPTOR_HEAP_MAX_NUM_DESCRIPTORS,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    CHECK_HRESULT(renderState.device->CreateDescriptorHeap(&sharedHeapDesc, IID_PPV_ARGS(&renderState.sharedDescriptorHeap)));
    renderState.sharedDescriptorHeap->SetName(L"sharedDescriptorHeap");

    sharedDescHeapAlloc.init(renderState.device.Get(), renderState.sharedDescriptorHeap.Get());

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = NUM_FRAMES_IN_FLIGHT,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    };
    CHECK_HRESULT(renderState.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&renderState.rtvHeap)));
}

void initNvapi()
{
    NvAPI_Initialize();
    NvAPI_Unload();

    bool serSupported = false;
    NvAPI_D3D12_IsNvShaderExtnOpCodeSupported(renderState.device.Get(), NV_EXTN_OP_HIT_OBJECT_REORDER_THREAD, &serSupported);
    if (serSupported)
    {
        Logger::log("SER API supported");
        renderState.useSer = true;
        NvAPI_D3D12_SetNvShaderExtnSlotSpace(renderState.device.Get(), NV_SHADER_EXTN_SLOT, NV_SHADER_EXTN_REGISTER_SPACE);
    }
    else
    {
        Logger::logWarning("SER API not supported");
        renderState.useSer = false;
    }
}

void initSwapChain()
{
    BOOL _allowTearing = FALSE;
    {
        CHECK_HRESULT(
            renderState.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &_allowTearing, sizeof(_allowTearing)));
    }
    renderState.allowTearing = bool(_allowTearing);

    renderState.useVsync = SettingsManager::getAsBool("useVsync");

    Logger::log("Use VSync: %s", renderState.useVsync ? "true" : "false");
    if (!renderState.useVsync)
    {
        Logger::log("Allow tearing: %s", renderState.allowTearing ? "true" : "false");
    }

    // Loading the plugin before the first swap chain exists means the setting's default does not
    // cost a swap chain rebuild on the first frame
    if (renderState.frameGen.supported)
    {
        loadFrameGenPlugin(isFrameGenerationRequested());
    }

    createSwapChain();

    CHECK_HRESULT(renderState.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES));
}

// The factories are kept alive past init so the swap chain can be rebuilt when frame generation
// is toggled; see setFrameGenerationActive. Interpolation itself is switched on at the end of
// resize(), which always follows a swap chain creation.
void createSwapChain()
{
    renderState.swapChainFlags = 0;
    if (isWaitableSwapChainActive())
    {
        renderState.swapChainFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }
    if (renderState.allowTearing)
    {
        renderState.swapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    DXGI_SWAP_CHAIN_DESC1 scDesc = {
        .Format = SWAP_CHAIN_FORMAT,
        .SampleDesc = SAMPLE_DESC_NO_AA,
        .BufferCount = NUM_FRAMES_IN_FLIGHT,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .Flags = renderState.swapChainFlags,
    };

    ComPtr<IDXGISwapChain1> proxySwapChain1;
    CHECK_HRESULT(renderState.proxyFactory->CreateSwapChainForHwnd(renderState.graphicsCmdQueue.Get(), hwnd, &scDesc, nullptr, nullptr, &proxySwapChain1));
    CHECK_HRESULT(proxySwapChain1.As(&renderState.proxySwapChain));
    CHECK_SL_RESULT(slGetNativeInterface(renderState.proxySwapChain.Get(),
                                         reinterpret_cast<void**>(renderState.swapChain.GetAddressOf())));
}

void releaseSwapChain()
{
    // The proxy wraps the native swap chain, so it has to go first
    renderState.proxySwapChain.Reset();
    renderState.swapChain.Reset();
}

bool isFrameGenerationRequested()
{
    const AntialiasingMode antialiasingMode =
        static_cast<AntialiasingMode>(SettingsManager::getAsUint("antialiasingMode"));
    return renderState.frameGen.supported && antialiasingMode == AntialiasingMode::DLSS
        && SettingsManager::getAsBool("frameGeneration");
}

void closeFrameLatencyWaitable()
{
    if (renderState.frameLatencyWaitable)
    {
        CloseHandle(renderState.frameLatencyWaitable);
        renderState.frameLatencyWaitable = nullptr;
    }
}

void setFrameGenerationActive(bool active)
{
    if (renderState.frameGen.active == active)
    {
        return;
    }

    flush();

    closeFrameLatencyWaitable();
    releaseSwapChain();

    loadFrameGenPlugin(active);

    createSwapChain();

    // The resize re-fetches the back buffers and their RTVs from the new swap chain; queueing it
    // rather than calling it here lets render() run a single resize even when one was already pending
    renderState.needsResize = true;
}

void initRtTargets()
{
    renderState.autoTransitionRtTargets.push_back(&renderState.pathTracingTarget);
    renderState.autoTransitionRtTargets.push_back(&renderState.diffuseAlbedoTarget);
    renderState.autoTransitionRtTargets.push_back(&renderState.specularAlbedoTarget);
    renderState.autoTransitionRtTargets.push_back(&renderState.depthTarget);
    renderState.autoTransitionRtTargets.push_back(&renderState.normalsAndRoughnessTarget);
    renderState.autoTransitionRtTargets.push_back(&renderState.motionTarget);
    renderState.autoTransitionRtTargets.push_back(&renderState.specularHitDistanceTarget);

    renderState.autoTransitionRtTargets.push_back(&renderState.dlssOutputTarget);

    renderState.autoTransitionRtTargets.push_back(&renderState.debugTarget);

    for (RtTarget* rtTarget : renderState.autoTransitionRtTargets)
    {
        renderState.allRtTargets.push_back(rtTarget);
    }

    // Written by a copy rather than a shader, so its transitions are driven by hand in render()
    renderState.allRtTargets.push_back(&renderState.hudlessTarget);

    resize();
}

void initCommand()
{
    for (auto& frame : renderState.frameCtxs)
    {
        CHECK_HRESULT(renderState.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.cmdAlloc)));
    }

    CHECK_HRESULT(renderState.device->CreateCommandList1(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&renderState.cmdList)));
    renderState.cmdList->SetName(L"main cmdList");
}

void initConstantParams()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, std::numeric_limits<uint32_t>::max());
    const uint32_t requestedSeed = SettingsManager::getAsUint("rngSeed");
    const uint32_t rngSeed = requestedSeed ? requestedSeed : dist(gen);

    for (auto& frame : renderState.frameCtxs)
    {
        auto& constantParams = frame.paramBlockManager.constantParams;
        constantParams->rngSeed = rngSeed;
    }
}

} // namespace Renderer
