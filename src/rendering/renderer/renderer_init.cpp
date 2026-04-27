// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include <random>

#include <sl_security.h>

#include <nvapi.h>
#include <nvShaderExtnEnums.h>
#undef min
#undef max

#include "rendering/window_manager.h"
#include "rendering/buffer/buffer_helper.h"
#include "settings_manager.h"
#include "logger.h"
#include "util/util.h"

using WindowManager::hwnd;

namespace Renderer
{

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
    prefs.logLevel = renderState.testMode ? sl::LogLevel::eOff : sl::LogLevel::eDefault;

    if (SettingsManager::getAsBool("verboseLogging"))
    {
        prefs.showConsole = true;
        prefs.logLevel = sl::LogLevel::eVerbose;
    }

    const sl::Feature features[] = { sl::kFeatureDLSS_RR };
    prefs.featuresToLoad = features;
    prefs.numFeaturesToLoad = _countof(features);

    prefs.applicationId = 1738; // TODO: not sure what to put here lol

    prefs.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    prefs.flags |= sl::PreferenceFlags::eUseManualHooking;

    CHECK_SL_RESULT(slInit(prefs));
}

void initDevice()
{
    const std::string slInterposerDllPath = std::string(TARGET_FILE_DIR) + "/sl.interposer.dll";
    const auto slMod = LoadLibrary(slInterposerDllPath.c_str());

    //typedef HRESULT(WINAPI * PFunCreateDXGIFactory)(REFIID, void**);
    //typedef HRESULT(WINAPI * PFunCreateDXGIFactory1)(REFIID, void**);
    typedef HRESULT(WINAPI * PFunCreateDXGIFactory2)(UINT, REFIID, void**);
    //typedef HRESULT(WINAPI * PFunDXGIGetDebugInterface1)(UINT, REFIID, void**);
    typedef HRESULT(WINAPI * PFunD3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

    //const auto slCreateDXGIFactory = reinterpret_cast<PFunCreateDXGIFactory>(GetProcAddress(slMod, "CreateDXGIFactory"));
    //const auto slCreateDXGIFactory1 = reinterpret_cast<PFunCreateDXGIFactory1>(GetProcAddress(slMod, "CreateDXGIFactory1"));
    const auto slCreateDXGIFactory2 = reinterpret_cast<PFunCreateDXGIFactory2>(GetProcAddress(slMod, "CreateDXGIFactory2"));
    //const auto slDXGIGetDebugInterface1 = reinterpret_cast<PFunDXGIGetDebugInterface1>(GetProcAddress(slMod, "DXGIGetDebugInterface1"));
    const auto slD3D12CreateDevice = reinterpret_cast<PFunD3D12CreateDevice>(GetProcAddress(slMod, "D3D12CreateDevice"));

    UINT dxgiFactoryFlags = 0;

    if (SettingsManager::getAsBool("gpuValidation"))
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
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

    ComPtr<IDXGIFactory2> proxyFactory2;
    CHECK_HRESULT(slCreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&proxyFactory2)));
    CHECK_HRESULT(proxyFactory2.As(&renderState.proxyFactory));
    CHECK_SL_RESULT(slGetNativeInterface(renderState.proxyFactory.Get(),
                                         reinterpret_cast<void**>(renderState.factory.GetAddressOf())));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; renderState.factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc;
        CHECK_HRESULT(adapter->GetDesc1(&desc));
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            adapter.Reset();
            continue;
        }

        if (SUCCEEDED(slD3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&renderState.proxyDevice))))
        {
            CHECK_SL_RESULT(slGetNativeInterface(renderState.proxyDevice.Get(),
                                                 reinterpret_cast<void**>(renderState.device.GetAddressOf())));
            CHECK_SL_RESULT(slSetD3DDevice(renderState.device.Get()));

            sl::AdapterInfo adapterInfo{};
            adapterInfo.deviceLUID = (uint8_t*)&desc.AdapterLuid;
            adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

            CHECK_SL_RESULT(slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo));

            Logger::log("Selected adapter: %ls", desc.Description);
            break;
        }

        adapter.Reset();
    }

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

    renderState.swapChainFlags = 0;
    if (renderState.useWaitableSwapChain)
    {
        renderState.swapChainFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }
    if (renderState.allowTearing)
    {
        renderState.swapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    DXGI_SWAP_CHAIN_DESC1 scDesc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
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

    CHECK_HRESULT(renderState.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES));

    renderState.factory.Reset();
    renderState.proxyFactory.Reset();
}

void initRtTargets()
{
    renderState.autoTransitionRtTargets.push_back(&renderState.pathTracingTarget);
    renderState.autoTransitionRtTargets.push_back(&renderState.diffuseAlbedoTarget);
    renderState.autoTransitionRtTargets.push_back(&renderState.specularAlbedoTarget);
    renderState.autoTransitionRtTargets.push_back(&renderState.linearDepthTarget);
    renderState.autoTransitionRtTargets.push_back(&renderState.normalsAndRoughnessTarget);
    renderState.autoTransitionRtTargets.push_back(&renderState.motionTarget);
    renderState.autoTransitionRtTargets.push_back(&renderState.specularHitDistanceTarget);

    renderState.autoTransitionRtTargets.push_back(&renderState.dlssOutputTarget);

    renderState.autoTransitionRtTargets.push_back(&renderState.debugTarget);

    for (RtTarget* rtTarget : renderState.autoTransitionRtTargets)
    {
        renderState.allRtTargets.push_back(rtTarget);
    }

    // nrcDebugTarget is added after the copy so it's in autoTransitionRtTargets but NOT in
    // allRtTargets — it has custom dimensions and is handled separately in resize().
    renderState.autoTransitionRtTargets.push_back(&renderState.nrcDebugTarget);

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
    const uint32_t rngSeed = dist(gen);

    for (auto& frame : renderState.frameCtxs)
    {
        auto& constantParams = frame.paramBlockManager.constantParams;
        constantParams->rngSeed = rngSeed;
    }
}

} // namespace Renderer
