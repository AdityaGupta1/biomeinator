/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "renderer.h"

#include "dxr_common.h"
#include <d3dcompiler.h>

#include "camera.h"
#include "param_block_manager.h"
#include "pipeline_builder.h"
#include "rt_target.h"
#include "settings_manager.h"
#include "settings_gui_helpers.h"
#include "window_manager.h"
#include "buffer/acs_helper.h"
#include "buffer/buffer_helper.h"
#include "buffer/managed_buffer.h"
#include "buffer/to_free_list.h"
#include "common/common_enums.h"
#include "common/common_hitgroups.h"
#include "common/common_registers.h"
#include "common/common_settings.h"
#include "scene/gltf_loader.h"
#include "scene/scene.h"
#include "util/util.h"

#include <chrono>
#include <random>
#include <deque>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <vector>
#include <shlobj.h>

#include "logger.h"

#include <stb_image_write.h>

#include "gbuffer.rgs.fxh"
#include "path_tracing.rgs.fxh"
#include "collect.cs.fxh"
#include "postprocess.vs.fxh"
#include "postprocess.ps.fxh"

#define SHARED_DESCRIPTOR_HEAP_MAX_NUM_DESCRIPTORS 64

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_d.h>
#include <sl_security.h>

#include <nvapi.h>
#include <nvShaderExtnEnums.h>

#ifdef _DEBUG
static void printSlResultError(sl::Result result)
{
    std::string msg;

    switch (result)
    {
        case sl::Result::eErrorNoPlugins:
            msg = "No plugins found";
            break;
        case sl::Result::eErrorInvalidParameter:
            msg = "Invalid parameter";
            break;
        case sl::Result::eErrorMissingConstants:
            msg = "Missing constants";
            break;
        default:
            msg = "Unknown Streamline error: " + std::to_string(static_cast<uint32_t>(result));
            break;
    }

    Logger::logError(msg.c_str());
}

#define CHECK_SL_RESULT(expr)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        if (SL_FAILED(result, expr))                                                                                   \
        {                                                                                                              \
            Logger::logError("sl::Result failed: %s", #expr);                                                          \
            printSlResultError(result);                                                                                \
            __debugbreak();                                                                                            \
        }                                                                                                              \
    } while (0)
#else
#define CHECK_SL_RESULT(expr) expr
#endif

using namespace DirectX;

using WindowManager::hwnd;

namespace Renderer
{

static void initStreamline();
static void initDevice();
static void initDescriptorHeaps();
static void initNvapi();
static void initSwapChain();
static void initRtTargets();
static void initCommand();
static void initConstantParams();
static void initRootSignature();
static void initPipeline();

static void initImgui();

static void beginFrame();
static void submitCmd();

static constexpr uint32_t NUM_FRAMES_IN_FLIGHT = 3;

struct FrameContext
{
    uint64_t fenceValue{ 0 };

    ComPtr<ID3D12CommandAllocator> cmdAlloc{ nullptr };
    ToFreeList toFreeList{};

    ParamBlockManager paramBlockManager{};
};

static FrameContext frameCtxs[NUM_FRAMES_IN_FLIGHT];
static uint32_t frameCtxIdx = 0;
static uint64_t nextFenceValue = 1;
static HANDLE fenceEvent;
static HANDLE frameLatencyWaitable;

static uint32_t frameNumber = 0;
static uint32_t accumulatedFrameNumber = 0;

static constexpr float defaultFovYDegrees = 35;
static Camera camera;

static ComPtr<ID3D12GraphicsCommandList4> cmdList;

static Scene scene;

static bool testMode = false;

void init()
{
    testMode = (SettingsManager::getAsString("testOutput") != "");

    initStreamline();

    initDevice();
    initDescriptorHeaps();

    initNvapi();

    for (uint32_t frameIdx = 0; frameIdx < NUM_FRAMES_IN_FLIGHT; ++frameIdx)
    {
        FrameContext& frameCtx = frameCtxs[frameIdx];
        frameCtx.paramBlockManager.init();
        frameCtx.paramBlockManager.setName(L"paramBlockManager " + std::to_wstring(frameIdx));
    }

    initSwapChain();
    initRtTargets();
    initCommand();
    initConstantParams();

    camera.init(XMConvertToRadians(defaultFovYDegrees));

    scene.init();

    initRootSignature();
    initPipeline();

    initImgui();

    const std::string& defaultScene = SettingsManager::getAsString("scene");
    if (!defaultScene.empty())
    {
        loadScene(defaultScene);
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
    dlssNeedsReset = true;
}

static void initStreamline()
{
    const std::wstring targetFileDirPath = Util::to_wstring(TARGET_FILE_DIR);
    const std::wstring slInterposerDllPath = targetFileDirPath + L"/sl.interposer.dll";

    // TODO: verify using WinVerifyTrust

    if (!sl::security::verifyEmbeddedSignature(slInterposerDllPath.c_str()))
    {
        Logger::logError("Could not verify signature of sl.interposer.dll");
        Logger::log("Exiting...");
        exit(-1);
    }

    sl::Preferences prefs = {};
    prefs.showConsole = false;
    prefs.logLevel = testMode ? sl::LogLevel::eOff : sl::LogLevel::eDefault;

#ifdef _DEBUG
    if (SettingsManager::getAsBool("verboseLogging"))
    {
        prefs.showConsole = true;
        prefs.logLevel = sl::LogLevel::eVerbose;
    }
#endif

    const sl::Feature features[] = { sl::kFeatureDLSS_RR };
    prefs.featuresToLoad = features;
    prefs.numFeaturesToLoad = _countof(features);

    prefs.applicationId = 1738; // TODO: not sure what to put here lol

    prefs.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;

    CHECK_SL_RESULT(slInit(prefs));
}

ComPtr<ID3D12Device5> device;

static ComPtr<IDXGIFactory4> factory;
static ComPtr<ID3D12CommandQueue> cmdQueue;
static ComPtr<ID3D12Fence> fence;
static void initDevice()
{
    const std::string slInterposerDllPath = std::string(TARGET_FILE_DIR) + "/sl.interposer.dll";
    const auto slMod = LoadLibrary(slInterposerDllPath.c_str());

    typedef HRESULT(WINAPI * PFunCreateDXGIFactory)(REFIID, void**);
    typedef HRESULT(WINAPI * PFunCreateDXGIFactory1)(REFIID, void**);
    typedef HRESULT(WINAPI * PFunCreateDXGIFactory2)(UINT, REFIID, void**);
    typedef HRESULT(WINAPI * PFunDXGIGetDebugInterface1)(UINT, REFIID, void**);
    typedef HRESULT(WINAPI * PFunD3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

    //const auto slCreateDXGIFactory = reinterpret_cast<PFunCreateDXGIFactory>(GetProcAddress(slMod, "CreateDXGIFactory"));
    //const auto slCreateDXGIFactory1 = reinterpret_cast<PFunCreateDXGIFactory1>(GetProcAddress(slMod, "CreateDXGIFactory1"));
    const auto slCreateDXGIFactory2 = reinterpret_cast<PFunCreateDXGIFactory2>(GetProcAddress(slMod, "CreateDXGIFactory2"));
    //const auto slDXGIGetDebugInterface1 = reinterpret_cast<PFunDXGIGetDebugInterface1>(GetProcAddress(slMod, "DXGIGetDebugInterface1"));
    const auto slD3D12CreateDevice = reinterpret_cast<PFunD3D12CreateDevice>(GetProcAddress(slMod, "D3D12CreateDevice"));

#ifdef _DEBUG
    ComPtr<ID3D12Debug> debug;
    CHECK_HRESULT(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    Logger::log("Enabled debug layer");
    debug->EnableDebugLayer();

    if (SettingsManager::getAsBool("gpuValidation"))
    {
        ComPtr<ID3D12Debug1> debug1;
        if (SUCCEEDED(debug.As(&debug1)))
        {
            debug1->SetEnableGPUBasedValidation(true);
        }
    }

#define DXGI_FACTORY_FLAGS DXGI_CREATE_FACTORY_DEBUG
#else
#define DXGI_FACTORY_FLAGS 0
#endif

    if (SUCCEEDED(slCreateDXGIFactory2(DXGI_FACTORY_FLAGS, IID_PPV_ARGS(&factory))))
    {
        Logger::log("Created factory");
    }

#undef DXGI_FACTORY_FLAGS

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc;
        CHECK_HRESULT(adapter->GetDesc1(&desc));
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            adapter.Reset();
            continue;
        }

        if (SUCCEEDED(slD3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device))))
        {
            CHECK_SL_RESULT(slSetD3DDevice(device.Get()));

            sl::AdapterInfo adapterInfo{};
            adapterInfo.deviceLUID = (uint8_t*)&desc.AdapterLuid;
            adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

            CHECK_SL_RESULT(slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo));

            Logger::log("Selected adapter: %ls", desc.Description);
            break;
        }

        adapter.Reset();
    }

    D3D12_COMMAND_QUEUE_DESC cmdQueueDesc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
    };
    CHECK_HRESULT(device->CreateCommandQueue(&cmdQueueDesc, IID_PPV_ARGS(&cmdQueue)));

    CHECK_HRESULT(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
}

static ComPtr<ID3D12DescriptorHeap> sharedDescriptorHeap;
DescriptorHeapAllocator sharedDescHeapAlloc;

static ComPtr<ID3D12DescriptorHeap> rtvHeap;

static void initDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC sharedHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = SHARED_DESCRIPTOR_HEAP_MAX_NUM_DESCRIPTORS,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    CHECK_HRESULT(device->CreateDescriptorHeap(&sharedHeapDesc, IID_PPV_ARGS(&sharedDescriptorHeap)));

    sharedDescHeapAlloc.init(device.Get(), sharedDescriptorHeap.Get());

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = NUM_FRAMES_IN_FLIGHT,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    };
    CHECK_HRESULT(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)));
}

static bool useSer = false;

void initNvapi()
{
    NvAPI_Initialize();
    NvAPI_Unload();

    bool serSupported = false;
    NvAPI_D3D12_IsNvShaderExtnOpCodeSupported(device.Get(), NV_EXTN_OP_HIT_OBJECT_REORDER_THREAD, &serSupported);
    if (serSupported)
    {
        useSer = true;
        NvAPI_D3D12_SetNvShaderExtnSlotSpace(device.Get(), NV_SHADER_EXTN_SLOT, NV_SHADER_EXTN_REGISTER_SPACE);
        Logger::log("SER enabled");
    }
    else
    {
        useSer = false;
        Logger::log("SER not supported");
    }
}

static ComPtr<IDXGISwapChain3> swapChain;
static constexpr uint32_t swapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

static void initSwapChain()
{
    DXGI_SWAP_CHAIN_DESC1 scDesc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = SAMPLE_DESC_NO_AA,
        .BufferCount = NUM_FRAMES_IN_FLIGHT,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .Flags = swapChainFlags,
    };
    ComPtr<IDXGISwapChain1> swapChain1;
    CHECK_HRESULT(factory->CreateSwapChainForHwnd(cmdQueue.Get(), hwnd, &scDesc, nullptr, nullptr, &swapChain1));
    CHECK_HRESULT(swapChain1.As(&swapChain));

    factory.Reset();
}

// clang-format off
static RtTarget pathTracingTarget{ L"pathTracingTarget", DXGI_FORMAT_R32G32B32A32_FLOAT, 3 };
static RtTarget diffuseAlbedoTarget{ L"diffuseAlbedoTarget", DXGI_FORMAT_R16G16B16A16_FLOAT, 3 };
static RtTarget specularAlbedoTarget{ L"specularAlbedoTarget", DXGI_FORMAT_R16G16B16A16_FLOAT, 3 };
static RtTarget linearDepthTarget{ L"linearDepthTarget", DXGI_FORMAT_R32_FLOAT, 1 };
// should really be 4 debug channels but it would be mostly transparent then
static RtTarget normalsAndRoughnessTarget{ L"normalsAndRoughnessTarget", DXGI_FORMAT_R16G16B16A16_FLOAT, 3 };
static RtTarget motionTarget{ L"motionTarget", DXGI_FORMAT_R16G16_FLOAT, 2 };
static RtTarget specularHitDistanceTarget{ L"specularHitDistanceTarget", DXGI_FORMAT_R32_FLOAT, 1 };

static RtTarget dlssOutputTarget{ L"dlssOutputTarget", DXGI_FORMAT_R32G32B32A32_FLOAT, 4, true };
static RtTarget debugTarget{ L"debugTarget", DXGI_FORMAT_R32G32B32A32_FLOAT, 4, true };
// clang-format on

static std::vector<RtTarget*> rtTargets;

static void initRtTargets()
{
    rtTargets.push_back(&pathTracingTarget);
    rtTargets.push_back(&diffuseAlbedoTarget);
    rtTargets.push_back(&specularAlbedoTarget);
    rtTargets.push_back(&linearDepthTarget);
    rtTargets.push_back(&normalsAndRoughnessTarget);
    rtTargets.push_back(&motionTarget);
    rtTargets.push_back(&specularHitDistanceTarget);

    rtTargets.push_back(&dlssOutputTarget);
    rtTargets.push_back(&debugTarget);

    resize();
}

static ComPtr<ID3D12Resource> dev_gbuffer;
static ComPtr<ID3D12Resource> dev_pathTracingRawBuffer;

static ComPtr<ID3D12Resource> dev_risSamples1;
static ComPtr<ID3D12Resource> dev_risSamples2;
static ID3D12Resource* dev_risSamplesIn;
static ID3D12Resource* dev_risSamplesOut;

static std::array<D3D12_CPU_DESCRIPTOR_HANDLE, NUM_FRAMES_IN_FLIGHT> rtvHeapCpuHandles;

static D3D12_VIEWPORT viewport;
static D3D12_RECT scissor;

static uint32_t renderWidth;
static uint32_t renderHeight;

static sl::ViewportHandle slViewportHandle{ 1738 }; // TODO: does this need to be a meaningful number?
static sl::Extent slRenderExtent;
static sl::Extent slViewportExtent;

static const std::vector<const char*> dlssModeOptions = {
    "DLAA", "quality", "balanced", "performance", "ultra performance",
};
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
    }

    flush();

    CHECK_HRESULT(swapChain->ResizeBuffers(0, viewportWidth, viewportHeight, DXGI_FORMAT_UNKNOWN, swapChainFlags));
    swapChain->SetMaximumFrameLatency(NUM_FRAMES_IN_FLIGHT - 1);
    frameLatencyWaitable = swapChain->GetFrameLatencyWaitableObject();

    dev_gbuffer.Reset();
    dev_gbuffer = BufferHelper::createBasicBuffer(renderWidth * renderHeight * sizeof(GbufferData),
                                                  &DEFAULT_HEAP,
                                                  { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    dev_gbuffer->SetName(L"dev_gbuffer");

    dev_risSamples1.Reset();
    dev_risSamples1 = BufferHelper::createBasicBuffer(renderWidth * renderHeight * sizeof(RisSample),
                                                      &DEFAULT_HEAP,
                                                      { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    dev_risSamples1->SetName(L"dev_risSamples1");
    dev_risSamplesIn = dev_risSamples1.Get();

    dev_risSamples2.Reset();
    dev_risSamples2 = BufferHelper::createBasicBuffer(renderWidth * renderHeight * sizeof(RisSample),
                                                      &DEFAULT_HEAP,
                                                      { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    dev_risSamples2->SetName(L"dev_risSamples2");
    dev_risSamplesOut = dev_risSamples2.Get();

    dev_pathTracingRawBuffer.Reset();
    const bool enablePathSplitting = SettingsManager::getAsBool("enablePathSplitting");
    const uint32_t pathTracingRawBufferSizeBytes =
        renderWidth * renderHeight * (enablePathSplitting ? 2 : 1) * sizeof(float) * 4;
    dev_pathTracingRawBuffer = BufferHelper::createBasicBuffer(
        pathTracingRawBufferSizeBytes, &DEFAULT_HEAP, { .resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS });
    dev_pathTracingRawBuffer->SetName(L"dev_pathTracingRawBuffer");

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

    for (RtTarget* rtTarget : rtTargets)
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
}

static void initCommand()
{
    for (auto& frame : frameCtxs)
    {
        CHECK_HRESULT(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.cmdAlloc)));
    }

    CHECK_HRESULT(device->CreateCommandList1(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&cmdList)));
    cmdList->SetName(L"main cmdList");

    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

static void initConstantParams()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, std::numeric_limits<uint32_t>::max());
    const uint32_t rngSeed = dist(gen);

    for (auto& frame : frameCtxs)
    {
        auto& constantParams = frame.paramBlockManager.constantParams;
        constantParams->rngSeed = rngSeed;
    }
}

enum class GbufferParam
{
    GLOBAL_PARAMS,

    RAYTRACING_ACS,
    VERTS,
    IDXS,
    INSTANCE_DATAS,
    MATERIALS,

    PER_TRI_DATAS,
    AREA_LIGHTS,
    AREA_LIGHT_SAMPLING_STRUCTURE,

    GBUFFER_OUT,
    RIS_SAMPLES_OUT,

    COUNT
};

enum class PtParam
{
    GLOBAL_PARAMS,

    RAYTRACING_ACS,
    VERTS,
    IDXS,
    INSTANCE_DATAS,
    MATERIALS,

    GBUFFER_IN,
    RIS_SAMPLES_IN,
    PER_TRI_DATAS,
    AREA_LIGHTS,
    AREA_LIGHT_SAMPLING_STRUCTURE,

    PATH_TRACING_RAW_BUFFER_OUT,

    COUNT
};

enum class PostprocessParam
{
    GLOBAL_PARAMS,

    COUNT
};

enum class CollectParam
{
    GLOBAL_PARAMS,

    PATH_TRACING_RAW_BUFFER_IN,

    COUNT
};

#define GBUFFER_PARAM_IDX(param) static_cast<uint32_t>(GbufferParam::param)
#define PT_PARAM_IDX(param) static_cast<uint32_t>(PtParam::param)
#define COLLECT_PARAM_IDX(param) static_cast<uint32_t>(CollectParam::param)
#define POSTPROCESS_PARAM_IDX(param) static_cast<uint32_t>(PostprocessParam::param)

static D3D12_ROOT_PARAMETER1 makeParam(const D3D12_ROOT_PARAMETER_TYPE type,
                                       const uint32_t reg,
                                       const uint32_t regSpace)
{
    return {
        .ParameterType = type,
        .Descriptor = {
            .ShaderRegister = reg,
            .RegisterSpace = regSpace,
        },
    };
}

#define MAKE_PARAM(type, regPrefix, name)                                                                              \
    makeParam(D3D12_ROOT_PARAMETER_TYPE_##type, regPrefix##_REGISTER_##name, regPrefix##_REGISTER_SPACE)

static ComPtr<ID3D12RootSignature> gbufferRootSig;
static ComPtr<ID3D12RootSignature> ptRootSig;
static ComPtr<ID3D12RootSignature> collectRootSig;
static ComPtr<ID3D12RootSignature> postprocessRootSig;
static void initRootSignature()
{
    std::vector<D3D12_STATIC_SAMPLER_DESC> rtStaticSamplers;

    rtStaticSamplers.push_back({
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .ShaderRegister = RT_REGISTER_TEX_SAMPLER,
        .RegisterSpace = RT_REGISTER_SPACE,
    });

    // ===================================
    // GBUFFER
    // ===================================
    {
        std::array<D3D12_ROOT_PARAMETER1, GBUFFER_PARAM_IDX(COUNT)> gbufferParams;

        gbufferParams[GBUFFER_PARAM_IDX(GLOBAL_PARAMS)] = MAKE_PARAM(CBV, COMMON, GLOBAL_PARAMS);

        gbufferParams[GBUFFER_PARAM_IDX(RAYTRACING_ACS)] = MAKE_PARAM(SRV, RT, RAYTRACING_ACS);
        gbufferParams[GBUFFER_PARAM_IDX(VERTS)] = MAKE_PARAM(SRV, RT, VERTS);
        gbufferParams[GBUFFER_PARAM_IDX(IDXS)] = MAKE_PARAM(SRV, RT, IDXS);
        gbufferParams[GBUFFER_PARAM_IDX(INSTANCE_DATAS)] = MAKE_PARAM(SRV, RT, INSTANCE_DATAS);
        gbufferParams[GBUFFER_PARAM_IDX(MATERIALS)] = MAKE_PARAM(SRV, RT, MATERIALS);

        gbufferParams[GBUFFER_PARAM_IDX(PER_TRI_DATAS)] = MAKE_PARAM(SRV, PT, PER_TRI_DATAS);
        gbufferParams[GBUFFER_PARAM_IDX(AREA_LIGHTS)] = MAKE_PARAM(SRV, PT, AREA_LIGHTS);
        gbufferParams[GBUFFER_PARAM_IDX(AREA_LIGHT_SAMPLING_STRUCTURE)] = MAKE_PARAM(SRV, PT, AREA_LIGHT_SAMPLING_STRUCTURE);

        gbufferParams[GBUFFER_PARAM_IDX(GBUFFER_OUT)] = MAKE_PARAM(UAV, GBUFFER, GBUFFER_OUT);
        gbufferParams[GBUFFER_PARAM_IDX(RIS_SAMPLES_OUT)] = MAKE_PARAM(UAV, GBUFFER, RIS_SAMPLES_OUT);

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC gbufferRootSigDesc = {
            .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
            .Desc_1_1 = {
                .NumParameters = static_cast<uint32_t>(gbufferParams.size()),
                .pParameters = gbufferParams.data(),
                .NumStaticSamplers = static_cast<uint32_t>(rtStaticSamplers.size()),
                .pStaticSamplers = rtStaticSamplers.data(),
                .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
            },
        };

        ComPtr<ID3DBlob> blob, errorBlob;
        CHECK_HRESULT_WITH_ERROR_BLOB(D3D12SerializeVersionedRootSignature(&gbufferRootSigDesc, &blob, &errorBlob),
                                      errorBlob);
        CHECK_HRESULT(device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&gbufferRootSig)));
    }

    // ===================================
    // PATH TRACING
    // ===================================
    {
        std::vector<D3D12_ROOT_PARAMETER1> ptParams;
        ptParams.resize(PT_PARAM_IDX(COUNT));

        ptParams[PT_PARAM_IDX(GLOBAL_PARAMS)] = MAKE_PARAM(CBV, COMMON, GLOBAL_PARAMS);

        ptParams[PT_PARAM_IDX(RAYTRACING_ACS)] = MAKE_PARAM(SRV, RT, RAYTRACING_ACS);
        ptParams[PT_PARAM_IDX(VERTS)] = MAKE_PARAM(SRV, RT, VERTS);
        ptParams[PT_PARAM_IDX(IDXS)] = MAKE_PARAM(SRV, RT, IDXS);
        ptParams[PT_PARAM_IDX(INSTANCE_DATAS)] = MAKE_PARAM(SRV, RT, INSTANCE_DATAS);
        ptParams[PT_PARAM_IDX(MATERIALS)] = MAKE_PARAM(SRV, RT, MATERIALS);

        ptParams[PT_PARAM_IDX(GBUFFER_IN)] = MAKE_PARAM(SRV, PT, GBUFFER_IN);
        ptParams[PT_PARAM_IDX(RIS_SAMPLES_IN)] = MAKE_PARAM(SRV, PT, RIS_SAMPLES_IN);
        ptParams[PT_PARAM_IDX(PER_TRI_DATAS)] = MAKE_PARAM(SRV, PT, PER_TRI_DATAS);
        ptParams[PT_PARAM_IDX(AREA_LIGHTS)] = MAKE_PARAM(SRV, PT, AREA_LIGHTS);
        ptParams[PT_PARAM_IDX(AREA_LIGHT_SAMPLING_STRUCTURE)] = MAKE_PARAM(SRV, PT, AREA_LIGHT_SAMPLING_STRUCTURE);

        ptParams[PT_PARAM_IDX(PATH_TRACING_RAW_BUFFER_OUT)] = MAKE_PARAM(UAV, PT, PATH_TRACING_RAW_BUFFER_OUT);

        if (useSer)
        {
            const D3D12_DESCRIPTOR_RANGE1 serDescriptorRange = {
                .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                .NumDescriptors = 1,
                .BaseShaderRegister = NV_SHADER_EXTN_SLOT,
                .RegisterSpace = NV_SHADER_EXTN_REGISTER_SPACE,
                .Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE,
                .OffsetInDescriptorsFromTableStart = 0,
            };
            ptParams.push_back({
                .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
                .DescriptorTable = {
                    .NumDescriptorRanges = 1,
                    .pDescriptorRanges = &serDescriptorRange,
                },
            });
        }

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rtRootSigDesc = {
            .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
            .Desc_1_1 = {
                .NumParameters = static_cast<uint32_t>(ptParams.size()),
                .pParameters = ptParams.data(),
                .NumStaticSamplers = static_cast<uint32_t>(rtStaticSamplers.size()),
                .pStaticSamplers = rtStaticSamplers.data(),
                .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
            },
        };

        ComPtr<ID3DBlob> blob, errorBlob;
        CHECK_HRESULT_WITH_ERROR_BLOB(D3D12SerializeVersionedRootSignature(&rtRootSigDesc, &blob, &errorBlob),
                                      errorBlob);
        CHECK_HRESULT(
            device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&ptRootSig)));
    }

    // ===================================
    // COLLECT
    // ===================================
    {
        std::array<D3D12_ROOT_PARAMETER1, COLLECT_PARAM_IDX(COUNT)> collectParams;

        collectParams[COLLECT_PARAM_IDX(GLOBAL_PARAMS)] = MAKE_PARAM(CBV, COMMON, GLOBAL_PARAMS);
        collectParams[COLLECT_PARAM_IDX(PATH_TRACING_RAW_BUFFER_IN)] = MAKE_PARAM(SRV, COLLECT, PATH_TRACING_RAW_BUFFER_IN);

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC collectRootSigDesc = {
            .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
            .Desc_1_1 = {
                .NumParameters = static_cast<uint32_t>(collectParams.size()),
                .pParameters = collectParams.data(),
                .NumStaticSamplers = 0,
                .pStaticSamplers = nullptr,
                .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
            },
        };

        ComPtr<ID3DBlob> blob, errorBlob;
        CHECK_HRESULT_WITH_ERROR_BLOB(D3D12SerializeVersionedRootSignature(&collectRootSigDesc, &blob, &errorBlob),
                                      errorBlob);
        CHECK_HRESULT(device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&collectRootSig)));
    }

    // ===================================
    // POSTPROCESSING
    // ===================================
    {
        std::array<D3D12_ROOT_PARAMETER1, POSTPROCESS_PARAM_IDX(COUNT)> postprocessParams;

        postprocessParams[POSTPROCESS_PARAM_IDX(GLOBAL_PARAMS)] = MAKE_PARAM(CBV, COMMON, GLOBAL_PARAMS);

        std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;

        staticSamplers.push_back({
            .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .ShaderRegister = POSTPROCESS_REGISTER_TEX_SAMPLER,
            .RegisterSpace = POSTPROCESS_REGISTER_SPACE,
        });

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC postprocessRootSigDesc = {
            .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
            .Desc_1_1 = {
                .NumParameters = static_cast<uint32_t>(postprocessParams.size()),
                .pParameters = postprocessParams.data(),
                .NumStaticSamplers = static_cast<uint32_t>(staticSamplers.size()),
                .pStaticSamplers = staticSamplers.data(),
                .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
            },
        };

        ComPtr<ID3DBlob> blob, errorBlob;
        CHECK_HRESULT_WITH_ERROR_BLOB(D3D12SerializeVersionedRootSignature(&postprocessRootSigDesc, &blob, &errorBlob),
                                      errorBlob);
        CHECK_HRESULT(device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&postprocessRootSig)));
    }
}

static ComPtr<ID3D12StateObject> gbufferPso;
static ComPtr<ID3D12Resource> dev_gbufferShaderIds;
static D3D12_DISPATCH_RAYS_DESC gbufferDispatchDesc;

static ComPtr<ID3D12StateObject> ptPso;
static ComPtr<ID3D12Resource> dev_ptShaderIds;
static D3D12_DISPATCH_RAYS_DESC ptDispatchDesc;

static ComPtr<ID3D12PipelineState> collectPso;

static ComPtr<ID3D12PipelineState> postprocessPso;

static constexpr uint32_t maxPayloadSizeBytes = 96;

static void initPipeline()
{
    // ===================================
    // GBUFFER
    // ===================================
    {
        RtPipelineInputs gbufferPipelineInputs = {
            .name = L"gbuffer",
            .pso = gbufferPso,
            .dev_shaderIds = dev_gbufferShaderIds,
            .rgsShaderName = L"RayGeneration",
            .missShaderName = L"Miss",
            .dispatchDesc = gbufferDispatchDesc,
        };

        gbufferPipelineInputs.shaderBytecode = gbuffer_rgs_shaderBytecode;
        gbufferPipelineInputs.maxPayloadSizeBytes = maxPayloadSizeBytes;
        gbufferPipelineInputs.rootSig = gbufferRootSig.Get();

        gbufferPipelineInputs.hitGroups.resize(2);
        gbufferPipelineInputs.hitGroups[GBUFFER_HITGROUP_PRIMARY] = {
            .HitGroupExport = L"gbuffer_HitGroup_Primary",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .ClosestHitShaderImport = L"ClosestHit_Primary",
        };
        gbufferPipelineInputs.hitGroups[GBUFFER_HITGROUP_LIGHTS] = {
            .HitGroupExport = L"gbuffer_HitGroup_Lights",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .ClosestHitShaderImport = L"ClosestHit_Lights",
        };

        makeRtPipeline(gbufferPipelineInputs);
    }

    // ===================================
    // PATH TRACING
    // ===================================
    {
        RtPipelineInputs ptPipelineInputs = {
            .name = L"pathTracing",
            .pso = ptPso,
            .dev_shaderIds = dev_ptShaderIds,
            .rgsShaderName = L"RayGeneration",
            .missShaderName = L"Miss",
            .dispatchDesc = ptDispatchDesc,
        };

        ptPipelineInputs.shaderBytecode = path_tracing_rgs_shaderBytecode;
        ptPipelineInputs.maxPayloadSizeBytes = maxPayloadSizeBytes;
        ptPipelineInputs.rootSig = ptRootSig.Get();

        ptPipelineInputs.hitGroups.resize(2);
        ptPipelineInputs.hitGroups[PT_HITGROUP_PRIMARY] = {
            .HitGroupExport = L"pt_HitGroup_Primary",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .ClosestHitShaderImport = L"ClosestHit_Primary",
        };
        ptPipelineInputs.hitGroups[PT_HITGROUP_LIGHTS] = {
            .HitGroupExport = L"pt_HitGroup_Lights",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .ClosestHitShaderImport = L"ClosestHit_Lights",
        };

        makeRtPipeline(ptPipelineInputs);
    }

    // ===================================
    // COLLECT
    // ===================================
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = collectRootSig.Get();
        psoDesc.CS = makeShaderBytecode(collect_cs_shaderBytecode);
        CHECK_HRESULT(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&collectPso)));
        collectPso->SetName(L"collectPso");
    }

    // ===================================
    // POSTPROCESSING
    // ===================================
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = postprocessRootSig.Get();
        psoDesc.VS = makeShaderBytecode(postprocess_vs_shaderBytecode);
        psoDesc.PS = makeShaderBytecode(postprocess_ps_shaderBytecode);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = {
            .DepthEnable = FALSE,
            .StencilEnable = FALSE,
        };
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.InputLayout = { nullptr, 0 }; // no verts/idxs
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc = SAMPLE_DESC_NO_AA;
        CHECK_HRESULT(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&postprocessPso)));
        postprocessPso->SetName(L"postprocessPso");
    }
}

static void initImgui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = NULL;
    io.LogFilename = NULL;

    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplDX12_InitInfo imguiDX12InitInfo = {};
    imguiDX12InitInfo.Device = device.Get();
    imguiDX12InitInfo.CommandQueue = cmdQueue.Get();
    imguiDX12InitInfo.NumFramesInFlight = NUM_FRAMES_IN_FLIGHT;
    imguiDX12InitInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    imguiDX12InitInfo.SrvDescriptorHeap = sharedDescriptorHeap.Get();
    imguiDX12InitInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*,
                                            D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
                                            D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
    { sharedDescHeapAlloc.alloc(outCpuHandle, outGpuHandle); };
    imguiDX12InitInfo.SrvDescriptorFreeFn =
        [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
    { sharedDescHeapAlloc.free(cpuHandle, gpuHandle); };

    ImGui_ImplDX12_Init(&imguiDX12InitInfo);
}

static int frameCount = 0;
static double elapsedTime = 0.0;
static auto lastTimePoint = std::chrono::high_resolution_clock::now();
static int lastFps = 0;

static void updateFps(double deltaTime)
{
    frameCount++;
    elapsedTime += deltaTime;

    if (elapsedTime >= 1.0)
    {
        lastFps = frameCount;
        frameCount = 0;
        elapsedTime = 0.0;

        std::wstring title = L"Biomeinator - FPS: " + std::to_wstring(lastFps);
        SetWindowTextW(hwnd, title.c_str());
    }
}

struct ScreenshotRequest
{
    bool active{ false };
    ComPtr<ID3D12Resource> readbackBuffer{ nullptr };
    uint32_t width{ 0 };
    uint32_t height{ 0 };
    uint32_t rowPitchBytes{ 0 };
    uint32_t rowPitchBytesAligned{ 0 };
    bool useTestOutputPath{ false };
};

static ScreenshotRequest screenshotRequest{};

void queueScreenshot(const bool useTestOutputPath)
{
    screenshotRequest.active = true;
    screenshotRequest.useTestOutputPath = useTestOutputPath;
}

static void captureQueuedScreenshot()
{
    RECT rect;
    GetClientRect(hwnd, &rect);
    const uint32_t width = rect.right - rect.left;
    const uint32_t height = rect.bottom - rect.top;

    screenshotRequest.width = width;
    screenshotRequest.height = height;

    screenshotRequest.rowPitchBytes = width * 4;
    screenshotRequest.rowPitchBytesAligned =
        (screenshotRequest.rowPitchBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const uint32_t readbackSizeBytes = screenshotRequest.rowPitchBytesAligned * height;

    screenshotRequest.readbackBuffer = BufferHelper::createBasicBuffer(readbackSizeBytes, &READBACK_HEAP);

    ComPtr<ID3D12Resource> backBuffer;
    CHECK_HRESULT(swapChain->GetBuffer(swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer)));

    D3D12_TEXTURE_COPY_LOCATION srcLocation = {
        .pResource = backBuffer.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };

    D3D12_TEXTURE_COPY_LOCATION destLocation = {};
    destLocation.pResource = screenshotRequest.readbackBuffer.Get();
    destLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destLocation.PlacedFootprint = {
        .Offset = 0,
        .Footprint = {
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .Width = width,
            .Height = height,
            .Depth = 1,
            .RowPitch = screenshotRequest.rowPitchBytesAligned,
        },
    };

    BufferHelper::stateTransitionResourceBarrier(
        cmdList.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&destLocation, 0, 0, 0, &srcLocation, nullptr);
    BufferHelper::stateTransitionResourceBarrier(
        cmdList.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

static void finalizeQueuedScreenshot()
{
    flush();

    std::vector<uint8_t> pixels(screenshotRequest.width * screenshotRequest.height * 4);
    uint8_t* mapped = nullptr;
    screenshotRequest.readbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (uint32_t row = 0; row < screenshotRequest.height; ++row)
    {
        memcpy(pixels.data() + screenshotRequest.rowPitchBytes * row,
               mapped + screenshotRequest.rowPitchBytesAligned * row,
               screenshotRequest.rowPitchBytes);
    }
    screenshotRequest.readbackBuffer->Unmap(0, nullptr);

    std::filesystem::path path;
    if (screenshotRequest.useTestOutputPath)
    {
        path = std::filesystem::absolute(SettingsManager::getAsString("testOutput"));
    }
    else
    {
        wchar_t documentsPath[MAX_PATH];
        if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documentsPath)))
        {
            throw std::runtime_error("Failed to get screenshots directory");
        }

        const std::filesystem::path dir = std::filesystem::path(documentsPath) / "biomeinator" / "screenshots";

        SYSTEMTIME st{};
        GetLocalTime(&st);
        char fileName[64];
        sprintf_s(fileName,
                  "%04d.%02d.%02d_%02d-%02d-%02d.png",
                  st.wYear,
                  st.wMonth,
                  st.wDay,
                  st.wHour,
                  st.wMinute,
                  st.wSecond);

        path = dir / fileName;
    }

    std::filesystem::create_directories(path.parent_path());

    stbi_write_png(path.string().c_str(),
                   screenshotRequest.width,
                   screenshotRequest.height,
                   4,
                   pixels.data(),
                   screenshotRequest.width * 4);

    Logger::log("Saved screenshot to %s", path.generic_string().c_str());

    screenshotRequest.readbackBuffer.Reset();
    screenshotRequest = ScreenshotRequest();
}

static const std::vector<const char*> samplingModeComboOptions = {
    "naive",
    "MIS",
    "RIS",
    "ReSTIR",
};
static const std::vector<const char*> antialiasingModeComboOptions = {
    "none",
    "accumulate",
    "DLSS",
};
static const std::vector<const char*> tonemappingComboOptions = {
    "none",
    "standard",
    "AgX",
    "Khronos PBR neutral",
};
static const std::vector<const char*> debugViewComboOptions = {
    "off", "pathTracing", "diffuseAlbedo", "specularAlbedo", "linearDepth", "motion", "specularHitDistance", "normals", "debug",
};
static const std::unordered_map<std::string, RtTarget*> debugViewComboMap = {
    { "off", nullptr },

    { "pathTracing", &pathTracingTarget },
    { "diffuseAlbedo", &diffuseAlbedoTarget },
    { "specularAlbedo", &specularAlbedoTarget },
    { "linearDepth", &linearDepthTarget },
    { "motion", &motionTarget },
    { "specularHitDistance", &specularHitDistanceTarget },
    { "normals", &normalsAndRoughnessTarget },

    { "debug", &debugTarget },
};

static void imguiBeginFrame()
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

static bool needsResize = false;
static bool didPathTracingSettingsChange = false;

static void imguiEndFrame()
{
    didPathTracingSettingsChange = false;

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);

    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_AlwaysAutoResize);

    didPathTracingSettingsChange |= SettingsGuiHelpers::InputUint("Max path depth", "maxPathDepth", 1, 16);
    didPathTracingSettingsChange |= SettingsGuiHelpers::ComboUint("Sampling mode", "samplingMode", samplingModeComboOptions);
    SettingsGuiHelpers::ComboUint("Tonemapping", "tonemapping", tonemappingComboOptions);

    needsResize |= SettingsGuiHelpers::Checkbox("Enable path splitting", "enablePathSplitting");

    SettingsGuiHelpers::SectionTitle("Antialiasing");
    const bool didAntialiasingChange =
        SettingsGuiHelpers::ComboUint("Antialiasing mode", "antialiasingMode", antialiasingModeComboOptions);
    needsResize |= didAntialiasingChange; // technically should need resize only when switching to or from DLSS, but whatever
    didPathTracingSettingsChange |= didAntialiasingChange;
    const AntialiasingMode antialiasingMode =
        static_cast<AntialiasingMode>(SettingsManager::getAsUint("antialiasingMode"));

    if (antialiasingMode == AntialiasingMode::ACCUMULATE)
    {
        ImGui::Text("accumulated frames: %u", accumulatedFrameNumber);
        didPathTracingSettingsChange |= SettingsGuiHelpers::SliderUint("Max accumulated frames", "maxAccumulatedFrames", 1, 1024);
    }
    else if (antialiasingMode == AntialiasingMode::DLSS)
    {
        needsResize |= SettingsGuiHelpers::ComboUint("DLSS mode", "dlssMode", dlssModeOptions);
    }

    SettingsGuiHelpers::VerticalSpacing();

    if (ImGui::CollapsingHeader("Debug", ImGuiTreeNodeFlags_DefaultOpen))
    {
        SettingsGuiHelpers::VerticalSpacing();

        SettingsGuiHelpers::SectionTitle("Debug view");
        didPathTracingSettingsChange |= SettingsGuiHelpers::ComboString("Debug view", "debugView", debugViewComboOptions);
        didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Debug view scale", "debugViewScale", -1000.f, 1000.f);

        SettingsGuiHelpers::VerticalSpacing();

        SettingsGuiHelpers::SectionTitle("Debug parameters");
        didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Debug bool 0", "debugBool0");
        didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Debug bool 1", "debugBool1");
        didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Debug bool 2", "debugBool2");
        didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Debug bool 3", "debugBool3");
        didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Debug float 0", "debugFloat0", -100.f, 100.f);
        didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Debug float 1", "debugFloat1", -100.f, 100.f);
        didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Debug float 2", "debugFloat2", -100.f, 100.f);
        didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Debug float 3", "debugFloat3", -100.f, 100.f);
    }

    ImGui::End();

    if (frameNumber == 0)
    {
        ImGui::SetWindowFocus(NULL);
    }

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList.Get());
}

static inline sl::Resource makeSlResource(RtTarget* target)
{
    return {
        sl::ResourceType::eTex2d,
        target->getTarget(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    };
}

static inline void swapRisBuffers()
{
    std::swap(dev_risSamplesIn, dev_risSamplesOut);

    // TODO: this probably doesn't need to transition both resources in all cases (e.g. the first and last times they are used in a frame)
    BufferHelper::stateTransitionResourceBarrier(cmdList.Get(),
                                                 dev_risSamplesIn,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    BufferHelper::stateTransitionResourceBarrier(cmdList.Get(),
                                                 dev_risSamplesOut,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

static bool stopAccumulating = false;

void render()
{
    if (needsResize)
    {
        resize();
        needsResize = false;
    }

    if (!testMode)
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
    if (!testMode)
    {
        playerInput = WindowManager::getPlayerInput();
    }
    const bool didCameraChange = camera.update(deltaTime, playerInput);

    if (useDlss)
    {
        camera.copySlConstantsTo(&slConstants);
        CHECK_SL_RESULT(slSetConstants(slConstants, *frameToken, slViewportHandle));

        camera.copyMatricesToDlssOptions(&dlssdOptions.worldToCameraView, &dlssdOptions.cameraViewToWorld);
        CHECK_SL_RESULT(slDLSSDSetOptions(slViewportHandle, dlssdOptions));
    }

    camera.copyParamsTo(paramBlockManager.cameraParams);

    const bool didSceneChange = scene.update(cmdList.Get(), frameCtx.toFreeList);

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
    const bool enablePathSplitting = SettingsManager::getAsBool("enablePathSplitting");
    renderParams->enablePathSplitting = enablePathSplitting ? 1 : 0;
    renderParams->antialiasingMode = static_cast<uint32_t>(antialiasingMode);

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
        debugParams->debugOutputSrvIdx = debugOutputTarget->getSrvIdx();
        debugParams->debugOutputNumChannels = debugOutputTarget->debugOutputNumChannels;
    }
    debugParams->debugOutputScale = SettingsManager::getAsFloat("debugViewScale");

    debugParams->debugBool0 = SettingsManager::getAsBool("debugBool0");
    debugParams->debugBool1 = SettingsManager::getAsBool("debugBool1");
    debugParams->debugBool2 = SettingsManager::getAsBool("debugBool2");
    debugParams->debugBool3 = SettingsManager::getAsBool("debugBool3");

    debugParams->debugFloat0 = SettingsManager::getAsFloat("debugFloat0");
    debugParams->debugFloat1 = SettingsManager::getAsFloat("debugFloat1");
    debugParams->debugFloat2 = SettingsManager::getAsFloat("debugFloat2");
    debugParams->debugFloat3 = SettingsManager::getAsFloat("debugFloat3");

    auto& sceneParams = paramBlockManager.sceneParams;
    sceneParams->numAreaLights = scene.getNumAreaLights();

    ID3D12DescriptorHeap* const descHeaps[] = { sharedDescriptorHeap.Get() };

    if (scene.hasTlas() && (!stopAccumulating || antialiasingMode != AntialiasingMode::ACCUMULATE))
    {
        cmdList->SetDescriptorHeaps(std::size(descHeaps), descHeaps);

        // ===================================
        // GBUFFER
        // ===================================

        cmdList->SetPipelineState1(gbufferPso.Get());
        cmdList->SetComputeRootSignature(gbufferRootSig.Get());

        // clang-format off
        cmdList->SetComputeRootConstantBufferView(GBUFFER_PARAM_IDX(GLOBAL_PARAMS), paramBlockManager.getDevBuffer()->GetGPUVirtualAddress());

        cmdList->SetComputeRootShaderResourceView(GBUFFER_PARAM_IDX(RAYTRACING_ACS), scene.getDevTlasAddress());
        cmdList->SetComputeRootShaderResourceView(GBUFFER_PARAM_IDX(VERTS), scene.getDevVertsBufferAddress());
        cmdList->SetComputeRootShaderResourceView(GBUFFER_PARAM_IDX(IDXS), scene.getDevIdxsBufferAddress());
        cmdList->SetComputeRootShaderResourceView(GBUFFER_PARAM_IDX(INSTANCE_DATAS), scene.getDevInstanceDatasAddress());
        cmdList->SetComputeRootShaderResourceView(GBUFFER_PARAM_IDX(MATERIALS), scene.getDevMaterialsAddress());

        cmdList->SetComputeRootShaderResourceView(GBUFFER_PARAM_IDX(PER_TRI_DATAS), scene.getDevPerTriDatasBufferAddress());
        cmdList->SetComputeRootShaderResourceView(GBUFFER_PARAM_IDX(AREA_LIGHTS), scene.getDevAreaLightsBufferAddress());
        cmdList->SetComputeRootShaderResourceView(GBUFFER_PARAM_IDX(AREA_LIGHT_SAMPLING_STRUCTURE), scene.getDevAreaLightSamplingStructureAddress());

        cmdList->SetComputeRootUnorderedAccessView(GBUFFER_PARAM_IDX(GBUFFER_OUT), dev_gbuffer->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(GBUFFER_PARAM_IDX(RIS_SAMPLES_OUT), dev_risSamplesOut->GetGPUVirtualAddress());
        // clang-format on

        // this isn't strictly necessary as the RtTargets should be auto-promoted to UNORDERED_ACCESS on first access,
        // but it helps with state tracking (since otherwise the transition to PIXEL_SHADER_RESOURCE would complain that
        // the before state doesn't match reality)
        for (RtTarget* rtTarget : rtTargets)
        {
            if (rtTarget->hasUav)
            {
                rtTarget->transitionToState(cmdList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        }

        const D3D12_RESOURCE_DESC& pathTracingTargetDesc = pathTracingTarget.getTarget()->GetDesc();
        gbufferDispatchDesc.Width = static_cast<uint32_t>(pathTracingTargetDesc.Width);
        gbufferDispatchDesc.Height = pathTracingTargetDesc.Height;
        cmdList->DispatchRays(&gbufferDispatchDesc);

        swapRisBuffers();

        // ===================================
        // PATH TRACING
        // ===================================

        // dev_gbuffer should be auto-promoted to UNORDERED_ACCESS when first accessed by the gbuffer, and then should
        // decay back to COMMON after executing the command list
        BufferHelper::stateTransitionResourceBarrier(cmdList.Get(),
                                                     dev_gbuffer.Get(),
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        cmdList->SetPipelineState1(ptPso.Get());
        cmdList->SetComputeRootSignature(ptRootSig.Get());

        // clang-format off
        cmdList->SetComputeRootConstantBufferView(PT_PARAM_IDX(GLOBAL_PARAMS), paramBlockManager.getDevBuffer()->GetGPUVirtualAddress());

        cmdList->SetComputeRootShaderResourceView(PT_PARAM_IDX(RAYTRACING_ACS), scene.getDevTlasAddress());
        cmdList->SetComputeRootShaderResourceView(PT_PARAM_IDX(VERTS), scene.getDevVertsBufferAddress());
        cmdList->SetComputeRootShaderResourceView(PT_PARAM_IDX(IDXS), scene.getDevIdxsBufferAddress());
        cmdList->SetComputeRootShaderResourceView(PT_PARAM_IDX(INSTANCE_DATAS), scene.getDevInstanceDatasAddress());
        cmdList->SetComputeRootShaderResourceView(PT_PARAM_IDX(MATERIALS), scene.getDevMaterialsAddress());

        cmdList->SetComputeRootShaderResourceView(PT_PARAM_IDX(GBUFFER_IN), dev_gbuffer->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(PT_PARAM_IDX(RIS_SAMPLES_IN), dev_risSamplesIn->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(PT_PARAM_IDX(PER_TRI_DATAS), scene.getDevPerTriDatasBufferAddress());
        cmdList->SetComputeRootShaderResourceView(PT_PARAM_IDX(AREA_LIGHTS), scene.getDevAreaLightsBufferAddress());
        cmdList->SetComputeRootShaderResourceView(PT_PARAM_IDX(AREA_LIGHT_SAMPLING_STRUCTURE), scene.getDevAreaLightSamplingStructureAddress());

        cmdList->SetComputeRootUnorderedAccessView(PT_PARAM_IDX(PATH_TRACING_RAW_BUFFER_OUT), dev_pathTracingRawBuffer->GetGPUVirtualAddress());
        // clang-format on

        ptDispatchDesc.Width = gbufferDispatchDesc.Width * (enablePathSplitting ? 2 : 1);
        ptDispatchDesc.Height = gbufferDispatchDesc.Height;
        cmdList->DispatchRays(&ptDispatchDesc);

        // ===================================
        // COLLECT
        // ===================================

        BufferHelper::stateTransitionResourceBarrier(cmdList.Get(),
                                                     dev_pathTracingRawBuffer.Get(),
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        cmdList->SetPipelineState(collectPso.Get());
        cmdList->SetComputeRootSignature(collectRootSig.Get());

        cmdList->SetComputeRootConstantBufferView(COLLECT_PARAM_IDX(GLOBAL_PARAMS), paramBlockManager.getDevBuffer()->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(COLLECT_PARAM_IDX(PATH_TRACING_RAW_BUFFER_IN), dev_pathTracingRawBuffer->GetGPUVirtualAddress());

        const uint32_t dispatchWidth = Util::caclulateDispatchSize(ptDispatchDesc.Width, COLLECT_WORKGROUP_SIZE_X);
        const uint32_t dispatchHeight = Util::caclulateDispatchSize(ptDispatchDesc.Height, COLLECT_WORKGROUP_SIZE_Y);
        cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

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

    // ===================================
    // POSTPROCESSING
    // ===================================

    cmdList->SetDescriptorHeaps(std::size(descHeaps), descHeaps);

    cmdList->SetPipelineState(postprocessPso.Get());
    cmdList->SetGraphicsRootSignature(postprocessRootSig.Get());

    cmdList->SetGraphicsRootConstantBufferView(POSTPROCESS_PARAM_IDX(GLOBAL_PARAMS),
                                               paramBlockManager.getDevBuffer()->GetGPUVirtualAddress());

    ComPtr<ID3D12Resource> backBuffer;
    const uint32_t currentBackBufferIndex = swapChain->GetCurrentBackBufferIndex();
    CHECK_HRESULT(swapChain->GetBuffer(currentBackBufferIndex, IID_PPV_ARGS(&backBuffer)));

    BufferHelper::stateTransitionResourceBarrier(
        cmdList.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    for (RtTarget* rtTarget : rtTargets)
    {
        if (rtTarget->hasSrv)
        {
            rtTarget->transitionToState(cmdList.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
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

    if (!testMode)
    {
        imguiEndFrame();
    }

    BufferHelper::stateTransitionResourceBarrier(
        cmdList.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    submitCmd();

    const uint64_t fenceValue = nextFenceValue++;
    CHECK_HRESULT(cmdQueue->Signal(fence.Get(), fenceValue));
    frameCtx.fenceValue = fenceValue;

    const uint32_t syncInterval = testMode ? 0 : 1; // use 0 in test mode to render as fast as possible
    swapChain->Present(syncInterval, 0);

    ++frameNumber;
    frameCtxIdx = (frameCtxIdx + 1) % NUM_FRAMES_IN_FLIGHT;

    updateFps(deltaTime);

    if (screenshotRequest.active)
    {
        finalizeQueuedScreenshot(); // this calls flush()

        if (testMode)
        {
            exit(0);
        }
    }
}

static void waitForFence(const uint64_t fenceValue)
{
    if (fence->GetCompletedValue() < fenceValue)
    {
        CHECK_HRESULT(fence->SetEventOnCompletion(fenceValue, fenceEvent));
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

static void beginFrame()
{
    FrameContext& frame = frameCtxs[frameCtxIdx];

    WaitForSingleObject(frameLatencyWaitable, INFINITE);
    waitForFence(frame.fenceValue);

    frame.toFreeList.freeAll();
    CHECK_HRESULT(frame.cmdAlloc->Reset());
    CHECK_HRESULT(cmdList->Reset(frame.cmdAlloc.Get(), nullptr));
}

static void submitCmd()
{
    CHECK_HRESULT(cmdList->Close());
    cmdQueue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(cmdList.GetAddressOf()));
}

void flush()
{
    const uint64_t fenceValue = nextFenceValue++;
    CHECK_HRESULT(cmdQueue->Signal(fence.Get(), fenceValue));

    waitForFence(fenceValue);

    for (auto& frame : frameCtxs)
    {
        frame.fenceValue = 0;
        frame.toFreeList.freeAll();
    }
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
    ImGui::DestroyContext();

    scene.reset();
    AcsHelper::reset();

    for (RtTarget* rtTarget : rtTargets)
    {
        rtTarget->reset();
    }

    dev_gbuffer.Reset();
    dev_pathTracingRawBuffer.Reset();

    dev_risSamples1.Reset();
    dev_risSamples2.Reset();
    dev_risSamplesIn = nullptr;
    dev_risSamplesOut = nullptr;

    screenshotRequest.readbackBuffer.Reset();

    gbufferPso.Reset();
    ptPso.Reset();
    collectPso.Reset();
    postprocessPso.Reset();

    gbufferRootSig.Reset();
    ptRootSig.Reset();
    collectRootSig.Reset();
    postprocessRootSig.Reset();

    dev_gbufferShaderIds.Reset();
    dev_ptShaderIds.Reset();

    swapChain.Reset();
    rtvHeap.Reset();
    sharedDescriptorHeap.Reset();

    for (FrameContext& frameCtx : frameCtxs)
    {
        frameCtx.cmdAlloc.Reset();
        frameCtx.paramBlockManager.reset();
    }

    cmdList.Reset();

    fence.Reset();
    CloseHandle(fenceEvent);
    CloseHandle(frameLatencyWaitable);

    cmdQueue.Reset();
    factory.Reset();

#ifdef _DEBUG
    ComPtr<ID3D12DebugDevice> debugDevice;
    if (SUCCEEDED(device.As(&debugDevice)))
    {
        debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
    }
#endif

    device.Reset();
}

} // namespace Renderer
