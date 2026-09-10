// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <dxgi1_5.h>

#include "fence.h"
#include "param_block_manager.h"
#include "rendering/buffer/descriptor_heap_allocator.h"
#include "rendering/buffer/managed_buffer.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/common/common_registers.h"
#include "rendering/common/common_settings.h"
#include "rendering/common/sharc_protocol.h"
#include "rendering/dxr_common.h"
#include "rendering/renderer.h"
#include "rt_target.h"
#include "util/ring_buffer.h"

#include <array>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include <sl.h>
#include <sl_dlss_d.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include "rendering/camera.h"
#include "rendering/gpu_profiler.h"
#include "rendering/gpu_sort/gpu_radix_sort.h"
#include "rendering/light_tree_manager.h"
#include "scene/scene.h"

namespace Renderer
{

// =============================================
// Types
// =============================================

struct FrameContext
{
    uint64_t fenceValue{ 0 };

    ComPtr<ID3D12CommandAllocator> cmdAlloc{ nullptr };
    ToFreeList toFreeList{};

    ParamBlockManager paramBlockManager{};
};

struct FrameTimeMeasurement
{
    float frameIdx;
    float timeMs;
};

// =============================================
// CHECK_SL_RESULT macro
// =============================================

inline std::string slResultToString(sl::Result result)
{
    switch (result)
    {
        case sl::Result::eErrorNoPlugins:
            return "No plugins found";
        case sl::Result::eErrorInvalidParameter:
            return "Invalid parameter";
        case sl::Result::eErrorMissingConstants:
            return "Missing constants";
        case sl::Result::eWarnOutOfVRAM:
            return "Out of VRAM";
        case sl::Result::eErrorAdapterNotSupported:
        case sl::Result::eErrorNoSupportedAdapterFound:
            return "Adapter not supported";
        case sl::Result::eErrorOSDisabledHWS:
            return "Hardware-accelerated GPU Scheduling disabled";
        case sl::Result::eErrorDriverOutOfDate:
            return "Driver out of date";
        case sl::Result::eErrorOSOutOfDate:
            return "OS out of date";
        default:
            return "Unknown Streamline error: " + std::to_string(static_cast<uint32_t>(result));
    }
}

#if ENABLE_ASSERTS
inline void printSlResultError(sl::Result result)
{
    Logger::logError(slResultToString(result).c_str());
}

#define CHECK_SL_RESULT(expr)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        if (SL_FAILED(result, expr))                                                                                   \
        {                                                                                                              \
            Logger::logError("sl::Result failed: %s", #expr);                                                          \
            Renderer::printSlResultError(result);                                                                      \
            __debugbreak();                                                                                            \
        }                                                                                                              \
    } while (0)
#else
#define CHECK_SL_RESULT(expr) expr
#endif

// =============================================
// Constants
// =============================================

#define SHARED_DESCRIPTOR_HEAP_MAX_NUM_DESCRIPTORS 64

// =============================================
// Param enums
// =============================================

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
    PER_TRI_DATAS,
    AREA_LIGHTS,
    AREA_LIGHT_SAMPLING_STRUCTURE,

    GBUFFER_IN,

    PATH_TRACING_RAW_BUFFER_OUT,
    PT_DIFFUSE_ALBEDO_RAW_BUFFER_OUT,

    SHARC_HASHES,
    SHARC_ACCUMULATION,
    SHARC_RESOLVED,

    RTSL_LIGHT_TREE,
    RTSL_LIGHT_TO_LEAF,
    RTSL_LEAF_TO_LIGHT,

    COUNT
};

enum class PostprocessParam
{
    GLOBAL_PARAMS,

    COUNT
};

enum class DebugViewParam
{
    GLOBAL_PARAMS,

    COUNT
};

enum class CollectParam
{
    GLOBAL_PARAMS,

    PATH_TRACING_RAW_BUFFER_IN,
    PT_DIFFUSE_ALBEDO_RAW_BUFFER_IN,

    COUNT
};

#define GBUFFER_PARAM_IDX(param) static_cast<uint32_t>(GbufferParam::param)
#define PT_PARAM_IDX(param) static_cast<uint32_t>(PtParam::param)
#define COLLECT_PARAM_IDX(param) static_cast<uint32_t>(CollectParam::param)
#define POSTPROCESS_PARAM_IDX(param) static_cast<uint32_t>(PostprocessParam::param)
#define DEBUG_VIEW_PARAM_IDX(param) static_cast<uint32_t>(DebugViewParam::param)

inline D3D12_ROOT_PARAMETER1 makeParam(const D3D12_ROOT_PARAMETER_TYPE type,
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

void serializeAndCreateRootSignature(const D3D12_ROOT_PARAMETER1* params,
                                     uint32_t numParams,
                                     const D3D12_STATIC_SAMPLER_DESC* staticSamplers,
                                     uint32_t numStaticSamplers,
                                     ComPtr<ID3D12RootSignature>& outRootSig);

// =============================================
// Internal function declarations
// =============================================

void initStreamline();
void initDevice();
void initDescriptorHeaps();
void initNvapi();
void initSwapChain();
void createSwapChain();
void releaseSwapChain();
void closeFrameLatencyWaitable();
// Frame generation rides on DLSS mode: its checkbox only shows there, so it must not stay on
// invisibly in the other antialiasing modes. The setting itself is left alone so switching back
// to DLSS restores the user's choice.
bool isFrameGenerationRequested();
// Loads or unloads the DLSS-G plugin and queues the swap chain rebuild that makes it take effect
void setFrameGenerationActive(bool active);
void initRtTargets();
void initCommand();
void initConstantParams();
void initRootSignature();
void initPipeline();
void initImgui();
void imguiBeginFrame();
void imguiEndFrame(double deltaTime);
void updateFps(double deltaTime);
void captureQueuedScreenshot();
void finalizeQueuedScreenshot();

// Perf run lifecycle (renderer_perf.cpp); all no-ops unless --perfOutput is set
void perfRunInit();
void perfRunUpdate(bool sceneReady, bool didSceneChange);
// The CPU sample for a frame excludes the waits that throttle it (frame latency, fence, Present),
// so it measures the frame's own work rather than the frame rate
void perfRunBeginCpuFrame();
void perfRunEndCpuFrame();
void perfRunCollectTimings(uint32_t slotIdx);
bool perfRunIsDone();
void perfRunFinish();

// =============================================
// Shared state
// =============================================

struct DlssState
{
    bool needsReset{ false };
    float mipBias{ 0.f };
    sl::ViewportHandle viewportHandle{ 1738 };
    sl::Extent renderExtent{};
    sl::Extent viewportExtent{};
    sl::DLSSDOptions options{};
};

struct FrameGenState
{
    // DLSS-G additionally needs Reflex and PCL; all three are checked together at startup
    bool supported{ false };
    // Shown in the GUI while unsupported; empty once supported or in headless runs
    std::string unsupportedReason;
    // Only ever changes between frames, since flipping it recreates the swap chain
    bool active{ false };
    // Frames DLSS-G presented for the last app frame. Not simply 2 while frame generation is on:
    // the interpolated frame is dropped when presents go out of sync. 1 whenever it is off.
    uint32_t framesPresentedLastFrame{ 1 };
    sl::DLSSGOptions options{};

    // PCL Stats measures input sampling latency by posting this window message and timing how long
    // the app takes to answer it with a ping marker; 0 when PCL is not loaded
    uint32_t pclStatsWindowMessage{ 0 };
    // Set by the message pump, answered with the next frame's token since that frame picks up the input
    bool pclPingPending{ false };
};

enum class PerfPhase
{
    WAITING_FOR_SCENE,
    WARMUP,
    MEASURING,
    DONE,
};

struct PerfRunState
{
    bool active{ false };
    PerfPhase phase{ PerfPhase::WAITING_FOR_SCENE };
    std::chrono::steady_clock::time_point startTime{};
    std::chrono::steady_clock::time_point phaseStartTime{};
    std::chrono::steady_clock::time_point cpuFrameStart{};
    uint32_t phaseStartFrame{ 0 };
    uint32_t quietStreak{ 0 }; // consecutive frames without a scene change
    // Frames in [measureStartFrame, measureEndFrame) are measured; GPU timings arrive
    // NUM_FRAMES_IN_FLIGHT frames late, so this range is what decides which ones count.
    // Both start unbounded so nothing counts before measuring and everything counts during it
    uint32_t measureStartFrame{ UINT32_MAX };
    uint32_t measureEndFrame{ UINT32_MAX };
    bool timedOut{ false };
    bool stablePowerState{ false };
    std::vector<GpuProfiler::FrameTimings> gpuSamples;
    std::vector<double> cpuFrameMs;
};

struct ScreenshotRequest
{
    bool active{ false };
    ComPtr<ID3D12Resource> readbackBuffer{ nullptr };
    uint32_t width{ 0 };
    uint32_t height{ 0 };
    ComPtr<ID3D12Resource> radianceReadback;
    uint32_t radianceWidth{ 0 }, radianceHeight{ 0 }, radianceSplits{ 1 };
    float radianceDivisor{ 1.f };
    uint32_t rowPitchBytes{ 0 };
    uint32_t rowPitchBytesAligned{ 0 };
    bool useTestOutputPath{ false };
};

// The back buffers and everything that has to match them exactly (PSO render target formats, the
// hudless copy, the screenshot readback footprint)
inline constexpr DXGI_FORMAT SWAP_CHAIN_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

struct SharcResources
{
    bool supported{ false };
    bool resetRequested{ true };
    bool wasEnabled{ false };
    uint32_t capacity{ 0 };
    uint32_t frameIndex{ 0 };
    DirectX::XMINT3 origin{};
    DirectX::XMFLOAT3 previousCamera{};
    float previousScale{ 0.f };
    ComPtr<ID3D12Resource> hashes, accumulation, resolved;
    ComPtr<ID3D12RootSignature> computeRootSig;
    ComPtr<ID3D12PipelineState> maintenancePso;
    ComPtr<ID3D12StateObject> updatePso, queryPso, diagnosticPso;
    ComPtr<ID3D12Resource> updateShaderIds, queryShaderIds, diagnosticShaderIds;
    D3D12_DISPATCH_RAYS_DESC updateDispatch{}, queryDispatch{}, diagnosticDispatch{};
};

void sharcInit();
void sharcPrepare(ParamBlockManager& params, bool sceneChanged);
void sharcMaintenance(ParamBlockManager& params, SharcMaintenanceMode mode);
void sharcBindPt();
void sharcDestroy();

struct RendererState
{
    RendererState();

    // -- Frame management --
    FrameContext frameCtxs[NUM_FRAMES_IN_FLIGHT];
    uint32_t frameNumber{ 0 };
    uint32_t accumulatedFrameNumber{ 0 };
    bool useWaitableSwapChain{ true };
    uint32_t frameCtxIdx{ 0 };
    HANDLE frameLatencyWaitable{ nullptr };
    std::chrono::high_resolution_clock::time_point lastTimePoint{ std::chrono::high_resolution_clock::now() };
    double animTime{ 0.0 }; // world animation time in seconds; advances unless paused, or at 50x while scrubbing
    float prevAnimTime{ 0.f }; // previous frame's RenderParams::animTime, for water motion vectors
    bool stopAccumulating{ false };

    // -- Device and infrastructure --
    ComPtr<IDXGIFactory5> factory;
    ComPtr<IDXGIFactory5> proxyFactory;
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12Device5> proxyDevice;
    ComPtr<ID3D12CommandQueue> graphicsCmdQueue;
    std::string adapterName;
    Fence fence;
    ComPtr<ID3D12DescriptorHeap> sharedDescriptorHeap;
    // sharedDescHeapAlloc is declared in rendering/renderer.h (public API)
    ComPtr<ID3D12DescriptorHeap> rtvHeap;

    // -- Command list --
    ComPtr<ID3D12GraphicsCommandList4> cmdList;

    // -- Scene --
    Scene scene;
    Camera camera;
    LightTreeManager lightTreeManager;
    SharcResources sharc;
    GpuRadixSort gpuRadixSort;

    // -- Mode flags --
    bool testMode{ false };
    bool headless{ false };
    bool voxelMode{ false };
    bool useSer{ false };
    // Voxel mode with raytracing tier 1.2: terrain alpha cutout resolves via opacity micromaps
    bool useOmms{ false };

    // -- Swap chain --
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<IDXGISwapChain3> proxySwapChain;
    UINT swapChainFlags{};
    bool useVsync{ false };
    bool allowTearing{ false };

    // -- Render targets --
    // clang-format off
    RtTarget pathTracingTarget{ L"pathTracingTarget", DXGI_FORMAT_R32G32B32A32_FLOAT, 3 };
    RtTarget diffuseAlbedoTarget{ L"diffuseAlbedoTarget", DXGI_FORMAT_R16G16B16A16_FLOAT, 3 };
    RtTarget specularAlbedoTarget{ L"specularAlbedoTarget", DXGI_FORMAT_R16G16B16A16_FLOAT, 3 };
    // Post-projection depth, which is what both DLSS-RR and DLSS-G ask for under kBufferTypeDepth
    RtTarget depthTarget{ L"depthTarget", DXGI_FORMAT_R32_FLOAT, 1 };
    // should really be 4 debug channels but it would look funny that way
    RtTarget normalsAndRoughnessTarget{ L"normalsAndRoughnessTarget", DXGI_FORMAT_R16G16B16A16_FLOAT, 3 };
    RtTarget motionTarget{ L"motionTarget", DXGI_FORMAT_R16G16_FLOAT, 2 };
    RtTarget specularHitDistanceTarget{ L"specularHitDistanceTarget", DXGI_FORMAT_R32_FLOAT, 1 };

    // Copy of the back buffer taken before the GUI is drawn, so frame generation can interpolate
    // the scene without the overlay smearing across generated frames. Only allocated while frame
    // generation is on. Keeps its SRV flag even though no shader reads it, since DLSS-G creates
    // its own views on the resource.
    RtTarget hudlessTarget{ L"hudlessTarget", SWAP_CHAIN_FORMAT, 0, true, false };

    RtTarget dlssOutputTarget{ L"dlssOutputTarget", DXGI_FORMAT_R32G32B32A32_FLOAT, 4, true };

    RtTarget debugTarget{ L"debugTarget", DXGI_FORMAT_R32G32B32A32_FLOAT, 4, true };
    // clang-format on

    std::vector<RtTarget*> allRtTargets;
    std::vector<RtTarget*> autoTransitionRtTargets;

    // -- Intermediate buffers --
    ComPtr<ID3D12Resource> dev_gbuffer;
    ComPtr<ID3D12Resource> dev_pathTracingRawBuffer;
    ComPtr<ID3D12Resource> dev_ptDiffuseAlbedoRawBuffer;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, NUM_FRAMES_IN_FLIGHT> rtvHeapCpuHandles{};

    // -- Viewport and dimensions --
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    uint32_t renderWidth{};
    uint32_t renderHeight{};

    // -- DLSS --
    DlssState dlss;
    FrameGenState frameGen;

    // -- Root signatures --
    ComPtr<ID3D12RootSignature> gbufferRootSig;
    ComPtr<ID3D12RootSignature> ptRootSig;
    ComPtr<ID3D12RootSignature> collectRootSig;
    ComPtr<ID3D12RootSignature> postprocessRootSig;
    ComPtr<ID3D12RootSignature> debugViewRootSig;

    // -- Pipeline state objects --
    ComPtr<ID3D12StateObject> gbufferPso;
    ComPtr<ID3D12Resource> dev_gbufferShaderIds;
    D3D12_DISPATCH_RAYS_DESC gbufferDispatchDesc{};

    ComPtr<ID3D12StateObject> ptPso;
    ComPtr<ID3D12Resource> dev_ptShaderIds;
    D3D12_DISPATCH_RAYS_DESC ptDispatchDesc{};

    ComPtr<ID3D12PipelineState> collectPso;

    ComPtr<ID3D12PipelineState> postprocessPso;
    ComPtr<ID3D12PipelineState> debugViewPso;

    // -- GUI shared state --
    bool needsResize{ false };
    bool didPathTracingSettingsChange{ false };
    RingBuffer<FrameTimeMeasurement, 600> frameTimeBuffer{};
    std::unordered_map<std::string, RtTarget*> debugViewComboMap;

    // -- Screenshot state --
    ScreenshotRequest screenshotRequest{};

    // -- Perf run state --
    PerfRunState perfRun{};
};

extern RendererState renderState;

// While the DLSS-G plugin is loaded, SL owns the swap chain's frame-latency waitable object and the
// app must stay off it; slReflexSleep paces the frame instead. See section 12.1 of the DLSS-G guide.
inline bool isWaitableSwapChainActive()
{
    return renderState.useWaitableSwapChain && !renderState.frameGen.active;
}

} // namespace Renderer
