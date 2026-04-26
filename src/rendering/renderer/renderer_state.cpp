// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include "rendering/camera.h"
#include "scene/scene.h"

namespace Renderer
{

// -- Frame management --
FrameContext frameCtxs[NUM_FRAMES_IN_FLIGHT];
uint32_t frameNumber = 0;
uint32_t accumulatedFrameNumber = 0;
bool useWaitableSwapChain = true;

// -- Device and infrastructure --
ComPtr<IDXGIFactory5> factory;
ComPtr<ID3D12Device5> device;
ComPtr<ID3D12CommandQueue> graphicsCmdQueue;
Fence fence;
ComPtr<ID3D12DescriptorHeap> sharedDescriptorHeap;
DescriptorHeapAllocator sharedDescHeapAlloc;
ComPtr<ID3D12DescriptorHeap> rtvHeap;

// -- Command list --
ComPtr<ID3D12GraphicsCommandList4> cmdList;

// -- Scene --
Scene scene;
Camera camera;
nrc::d3d12::Context* nrcContext = nullptr;

// -- Mode flags --
bool testMode = false;
bool voxelMode = false;
bool useSer = false;

// -- Swap chain --
ComPtr<IDXGISwapChain3> swapChain;
UINT swapChainFlags;
bool useVsync = false;
bool allowTearing = false;

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

RtTarget nrcDebugTarget{ L"nrcDebugTarget", DXGI_FORMAT_R32G32B32A32_FLOAT, 3 };
// clang-format on

std::vector<RtTarget*> allRtTargets;
std::vector<RtTarget*> autoTransitionRtTargets;

// -- Viewport and dimensions --
D3D12_VIEWPORT viewport;
D3D12_RECT scissor;
uint32_t renderWidth;
uint32_t renderHeight;

// -- Root signatures --
ComPtr<ID3D12RootSignature> gbufferRootSig;
ComPtr<ID3D12RootSignature> ptRootSig;
ComPtr<ID3D12RootSignature> collectRootSig;
ComPtr<ID3D12RootSignature> nrcResolveRootSig;
ComPtr<ID3D12RootSignature> postprocessRootSig;
ComPtr<ID3D12RootSignature> debugViewRootSig;

// -- Pipeline state objects --
ComPtr<ID3D12StateObject> gbufferPso;
ComPtr<ID3D12Resource> dev_gbufferShaderIds;
D3D12_DISPATCH_RAYS_DESC gbufferDispatchDesc;

ComPtr<ID3D12StateObject> ptPso;
ComPtr<ID3D12Resource> dev_ptShaderIds;
D3D12_DISPATCH_RAYS_DESC ptDispatchDesc;

ComPtr<ID3D12PipelineState> collectPso;
ComPtr<ID3D12PipelineState> nrcResolvePso;

ComPtr<ID3D12StateObject> nrcUpdatePso;
ComPtr<ID3D12Resource> dev_nrcUpdateShaderIds;
D3D12_DISPATCH_RAYS_DESC nrcUpdateDispatchDesc;

ComPtr<ID3D12StateObject> nrcQueryPso;
ComPtr<ID3D12Resource> dev_nrcQueryShaderIds;
D3D12_DISPATCH_RAYS_DESC nrcQueryDispatchDesc;

ComPtr<ID3D12PipelineState> postprocessPso;
ComPtr<ID3D12PipelineState> debugViewPso;

// -- GUI shared state --
bool needsResize = false;
bool didPathTracingSettingsChange = false;
RingBuffer<FrameTimeMeasurement, 600> frameTimeBuffer{};
const std::unordered_map<std::string, RtTarget*> debugViewComboMap = {
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

// -- Screenshot state --
ScreenshotRequest screenshotRequest{};

} // namespace Renderer
