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
#include <string>
#include <unordered_map>
#include <vector>

#include <sl.h>
#include <sl_dlss_d.h>

class Camera;
class Scene;
namespace nrc { namespace d3d12 { class Context; } }

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

    NRC_CONSTANTS,

    NRC_QUERY_PATH_INFO,
    NRC_TRAINING_PATH_INFO,
    NRC_TRAINING_PATH_VERTICES,
    NRC_QUERY_RADIANCE_PARAMS,
    NRC_COUNTERS_DATA,

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

enum class NrcResolveParam
{
    NRC_CONSTANTS,

    QUERY_PATH_INFO,
    QUERY_RADIANCE,

    PATH_TRACING_RAW_BUFFER_OUT,

    COUNT
};

#define GBUFFER_PARAM_IDX(param) static_cast<uint32_t>(GbufferParam::param)
#define PT_PARAM_IDX(param) static_cast<uint32_t>(PtParam::param)
#define COLLECT_PARAM_IDX(param) static_cast<uint32_t>(CollectParam::param)
#define NRC_RESOLVE_PARAM_IDX(param) static_cast<uint32_t>(NrcResolveParam::param)
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
void configureNrc();
void initNrc();
void destroyNrc();
void initImgui();
void imguiBeginFrame();
void imguiEndFrame(double deltaTime);
void updateFps(double deltaTime);
void captureQueuedScreenshot();
void finalizeQueuedScreenshot();

// =============================================
// Shared state
// =============================================

// -- Frame management --
extern FrameContext frameCtxs[NUM_FRAMES_IN_FLIGHT];
extern uint32_t frameNumber;
extern uint32_t accumulatedFrameNumber;
extern bool useWaitableSwapChain;

// -- Device and infrastructure --
extern ComPtr<IDXGIFactory5> factory;
extern ComPtr<ID3D12Device5> device;
extern ComPtr<ID3D12CommandQueue> graphicsCmdQueue;
extern Fence fence;
extern ComPtr<ID3D12DescriptorHeap> sharedDescriptorHeap;
extern DescriptorHeapAllocator sharedDescHeapAlloc;
extern ComPtr<ID3D12DescriptorHeap> rtvHeap;

// -- Command list --
extern ComPtr<ID3D12GraphicsCommandList4> cmdList;

// -- Scene --
extern Scene scene;
extern Camera camera;
extern nrc::d3d12::Context* nrcContext;

// -- Mode flags --
extern bool testMode;
extern bool voxelMode;
extern bool useSer;

// -- Swap chain --
extern ComPtr<IDXGISwapChain3> swapChain;
extern UINT swapChainFlags;
extern bool useVsync;
extern bool allowTearing;

// -- Render targets --
extern RtTarget pathTracingTarget;
extern RtTarget diffuseAlbedoTarget;
extern RtTarget specularAlbedoTarget;
extern RtTarget linearDepthTarget;
extern RtTarget normalsAndRoughnessTarget;
extern RtTarget motionTarget;
extern RtTarget specularHitDistanceTarget;
extern RtTarget dlssOutputTarget;
extern RtTarget debugTarget;
extern RtTarget nrcDebugTarget;
extern std::vector<RtTarget*> allRtTargets;
extern std::vector<RtTarget*> autoTransitionRtTargets;

// -- Viewport and dimensions --
extern D3D12_VIEWPORT viewport;
extern D3D12_RECT scissor;
extern uint32_t renderWidth;
extern uint32_t renderHeight;

// -- Root signatures --
extern ComPtr<ID3D12RootSignature> gbufferRootSig;
extern ComPtr<ID3D12RootSignature> ptRootSig;
extern ComPtr<ID3D12RootSignature> collectRootSig;
extern ComPtr<ID3D12RootSignature> nrcResolveRootSig;
extern ComPtr<ID3D12RootSignature> postprocessRootSig;
extern ComPtr<ID3D12RootSignature> debugViewRootSig;

// -- Pipeline state objects --
extern ComPtr<ID3D12StateObject> gbufferPso;
extern ComPtr<ID3D12Resource> dev_gbufferShaderIds;
extern D3D12_DISPATCH_RAYS_DESC gbufferDispatchDesc;

extern ComPtr<ID3D12StateObject> ptPso;
extern ComPtr<ID3D12Resource> dev_ptShaderIds;
extern D3D12_DISPATCH_RAYS_DESC ptDispatchDesc;

extern ComPtr<ID3D12PipelineState> collectPso;
extern ComPtr<ID3D12PipelineState> nrcResolvePso;

extern ComPtr<ID3D12StateObject> nrcUpdatePso;
extern ComPtr<ID3D12Resource> dev_nrcUpdateShaderIds;
extern D3D12_DISPATCH_RAYS_DESC nrcUpdateDispatchDesc;

extern ComPtr<ID3D12StateObject> nrcQueryPso;
extern ComPtr<ID3D12Resource> dev_nrcQueryShaderIds;
extern D3D12_DISPATCH_RAYS_DESC nrcQueryDispatchDesc;

extern ComPtr<ID3D12PipelineState> postprocessPso;
extern ComPtr<ID3D12PipelineState> debugViewPso;

// -- GUI shared state --
extern bool needsResize;
extern bool didPathTracingSettingsChange;
extern RingBuffer<FrameTimeMeasurement, 600> frameTimeBuffer;
extern const std::unordered_map<std::string, RtTarget*> debugViewComboMap;

// -- Screenshot state --
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
extern ScreenshotRequest screenshotRequest;

} // namespace Renderer
