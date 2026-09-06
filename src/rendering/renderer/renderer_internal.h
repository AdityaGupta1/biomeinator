// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <dxgi1_5.h>

#include "rendering/dxr_common.h"
#include "rendering/renderer.h"
#include "fence.h"
#include "rt_target.h"
#include "param_block_manager.h"
#include "rendering/buffer/descriptor_heap_allocator.h"
#include "rendering/buffer/managed_buffer.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/common/common_registers.h"
#include "rendering/common/common_settings.h"
#include "util/ring_buffer.h"

#include <array>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include <sl.h>
#include <sl_dlss_d.h>

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

#if ENABLE_ASSERTS
inline void printSlResultError(sl::Result result)
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
        case sl::Result::eWarnOutOfVRAM:
            msg = "Out of VRAM";
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

    RTSL_LIGHT_TREE,
    RTSL_LIGHT_TO_LEAF,

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
void perfRunUpdate(bool sceneReady, bool didSceneChange, double deltaTime);
void perfRunOnFrameTimings(const GpuProfiler::FrameTimings& timings);
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
    uint32_t phaseStartFrame{ 0 };
    uint32_t quietStreak{ 0 }; // consecutive frames without a scene change
    // Frames in [measureStartFrame, measureEndFrame) are measured; GPU timings arrive
    // NUM_FRAMES_IN_FLIGHT frames late, so this range is what decides which ones count
    uint32_t measureStartFrame{ 0 };
    uint32_t measureEndFrame{ 0 };
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
    uint32_t rowPitchBytes{ 0 };
    uint32_t rowPitchBytesAligned{ 0 };
    bool useTestOutputPath{ false };
};

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
    RtTarget linearDepthTarget{ L"linearDepthTarget", DXGI_FORMAT_R32_FLOAT, 1 };
    // should really be 4 debug channels but it would look funny that way
    RtTarget normalsAndRoughnessTarget{ L"normalsAndRoughnessTarget", DXGI_FORMAT_R16G16B16A16_FLOAT, 3 };
    RtTarget motionTarget{ L"motionTarget", DXGI_FORMAT_R16G16_FLOAT, 2 };
    RtTarget specularHitDistanceTarget{ L"specularHitDistanceTarget", DXGI_FORMAT_R32_FLOAT, 1 };

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

} // namespace Renderer
