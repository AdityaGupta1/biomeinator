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

#include "param_block_manager.h"
#include "settings_manager.h"
#include "window_manager.h"
#include "buffer/acs_helper.h"
#include "buffer/buffer_helper.h"
#include "buffer/managed_buffer.h"
#include "buffer/to_free_list.h"
#include "common/common_hitgroups.h"
#include "common/common_registers.h"
#include "scene/camera.h"
#include "scene/gltf_loader.h"
#include "scene/scene.h"

#include <chrono>
#include <random>
#include <deque>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <shlobj.h>

#include <stb_image_write.h>

#include "main.fxh"

using namespace DirectX;

using WindowManager::hwnd;

namespace Renderer
{

void initDevice();
void initDescriptorHeaps();
void initRenderTarget();
void initCommand();
void initConstantParams();
void initRootSignature();
void compileShadersAndInitPipeline();

void beginFrame();
void submitCmd();

constexpr uint32_t NUM_FRAMES_IN_FLIGHT = 3;

struct FrameContext
{
    uint64_t fenceValue{ 0 };

    ComPtr<ID3D12CommandAllocator> cmdAlloc{ nullptr };
    ToFreeList toFreeList{};

    ParamBlockManager paramBlockManager{};
};

FrameContext frameCtxs[NUM_FRAMES_IN_FLIGHT];
uint32_t frameCtxIdx = 0;
uint64_t nextFenceValue = 1;
HANDLE fenceEvent;
HANDLE frameLatencyWaitable;

uint32_t frameNumber = 0;

constexpr float defaultFovYDegrees = 35;
Camera camera;

ComPtr<ID3D12GraphicsCommandList4> cmdList;

Scene scene;

bool testMode = false;

void init()
{
    initDevice();
    initDescriptorHeaps();

    for (auto& frame : frameCtxs)
    {
        frame.paramBlockManager.init();
    }

    initRenderTarget();
    initCommand();
    initConstantParams();

    camera.init(XMConvertToRadians(defaultFovYDegrees));

    scene.init();

    initRootSignature();
    compileShadersAndInitPipeline();

    const std::string& defaultScene = SettingsManager::getAsString("scene");
    if (!defaultScene.empty())
    {
        loadGltf(defaultScene);
    }

    testMode = (SettingsManager::getAsString("testOutput") != "");
    if (!testMode)
    {
        SetForegroundWindow(hwnd);
    }
}

void loadGltf(const std::string& filePathStr)
{
    flush();
    GltfLoader::loadGltf(filePathStr, scene);
}

ComPtr<IDXGIFactory4> factory;
ComPtr<ID3D12Device5> device;
ComPtr<ID3D12CommandQueue> cmdQueue;
ComPtr<ID3D12Fence> fence;
void initDevice()
{
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        printf("Enabled debug layer\n");
        debug->EnableDebugLayer();
    }

    if (SUCCEEDED(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory))))
    {
        printf("Created debug factory\n");
    }
    else
    {
        printf("Failed to create debug factory, falling back to non-debug\n");
    }
#endif

    if (!factory)
    {
        CHECK_HRESULT(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
        printf("Created factory\n");
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            adapter.Reset();
            continue;
        }

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device))))
        {
            printf("Selected adapter: %ls\n", desc.Description);
            break;
        }

        adapter.Reset();
    }

    D3D12_COMMAND_QUEUE_DESC cmdQueueDesc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
    };
    device->CreateCommandQueue(&cmdQueueDesc, IID_PPV_ARGS(&cmdQueue));

    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
}

ComPtr<ID3D12DescriptorHeap> sharedDescriptorHeap;
DescriptorHeapAllocator sharedDescHeapAlloc;

void initDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC sharedHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = RESOURCE_DESCRIPTOR_HEAP_MAX_NUM_DESCRIPTORS,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    device->CreateDescriptorHeap(&sharedHeapDesc, IID_PPV_ARGS(&sharedDescriptorHeap));

    sharedDescHeapAlloc.init(device.Get(), sharedDescriptorHeap.Get());
}

ComPtr<IDXGISwapChain3> swapChain;
constexpr uint32_t swapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
void initRenderTarget()
{
    DXGI_SWAP_CHAIN_DESC1 scDesc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = NO_AA,
        .BufferCount = NUM_FRAMES_IN_FLIGHT,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .Flags = swapChainFlags,
    };
    ComPtr<IDXGISwapChain1> swapChain1;
    factory->CreateSwapChainForHwnd(cmdQueue.Get(), hwnd, &scDesc, nullptr, nullptr, &swapChain1);
    swapChain1.As(&swapChain);

    factory.Reset();

    resize();
}

ComPtr<ID3D12Resource> renderTarget;
void resize()
{
    if (!swapChain)
    {
        return;
    }

    RECT rect;
    GetClientRect(hwnd, &rect);
    const uint32_t width = std::max<uint32_t>(rect.right - rect.left, 1);
    const uint32_t height = std::max<uint32_t>(rect.bottom - rect.top, 1);

    flush();

    swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapChainFlags);
    swapChain->SetMaximumFrameLatency(NUM_FRAMES_IN_FLIGHT - 1);
    frameLatencyWaitable = swapChain->GetFrameLatencyWaitableObject();

    if (renderTarget)
    {
        renderTarget.Reset();
    }

    const D3D12_RESOURCE_DESC renderTargetDesc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = NO_AA,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
    };
    device->CreateCommittedResource(&DEFAULT_HEAP,
                                    D3D12_HEAP_FLAG_NONE,
                                    &renderTargetDesc,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    nullptr,
                                    IID_PPV_ARGS(&renderTarget));

    const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
    };
    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle;
    uint32_t uavIdx = sharedDescHeapAlloc.alloc(&uavHandle);
    device->CreateUnorderedAccessView(renderTarget.Get(), nullptr, &uavDesc, uavHandle);

    for (auto& frame : frameCtxs)
    {
        frame.paramBlockManager.heapIndices->uav.renderTargetIdx = uavIdx;
    }
}

void initCommand()
{
    for (auto& frame : frameCtxs)
    {
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.cmdAlloc));
    }

    device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&cmdList));

    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void initConstantParams()
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

enum class RtParam
{
    GLOBAL_PARAMS,
    RAYTRACING_ACS,
    IDXS,
    MATERIALS,
    AREA_LIGHT_SAMPLING_STRUCTURE,

    COUNT
};

#define RT_PARAM_IDX(rtParam) static_cast<uint32_t>(RtParam::rtParam)

ComPtr<ID3D12RootSignature> rtRootSig;
void initRootSignature()
{
    // ===================================
    // RAYTRACING
    // ===================================
    {
        std::array<D3D12_ROOT_PARAMETER1, RT_PARAM_IDX(COUNT)> rtParams;

        rtParams[RT_PARAM_IDX(GLOBAL_PARAMS)] = {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
            .Descriptor = {
                .ShaderRegister = RT_REGISTER_GLOBAL_PARAMS,
                .RegisterSpace = RT_REGISTER_SPACE_BUFFERS,
            },
        };

        rtParams[RT_PARAM_IDX(RAYTRACING_ACS)] = {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
            .Descriptor = {
                .ShaderRegister = RT_REGISTER_RAYTRACING_ACS,
                .RegisterSpace = RT_REGISTER_SPACE_BUFFERS,
            },
        };

        rtParams[RT_PARAM_IDX(IDXS)] = {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
            .Descriptor = {
                .ShaderRegister = RT_REGISTER_IDXS,
                .RegisterSpace = RT_REGISTER_SPACE_BUFFERS,
            },
        };

        rtParams[RT_PARAM_IDX(MATERIALS)] = {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
            .Descriptor = {
                .ShaderRegister = RT_REGISTER_MATERIALS,
                .RegisterSpace = RT_REGISTER_SPACE_BUFFERS,
            },
        };

        rtParams[RT_PARAM_IDX(AREA_LIGHT_SAMPLING_STRUCTURE)] = {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
            .Descriptor = {
                .ShaderRegister = RT_REGISTER_AREA_LIGHT_SAMPLING_STRUCTURE,
                .RegisterSpace = RT_REGISTER_SPACE_BUFFERS,
            },
        };

        std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;

        staticSamplers.push_back({
            .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .ShaderRegister = RT_REGISTER_TEX_SAMPLER,
            .RegisterSpace = RT_REGISTER_SPACE_TEXTURES,
        });

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rtRootSigDesc = {
            .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
            .Desc_1_1 = {
                .NumParameters = static_cast<uint32_t>(rtParams.size()),
                .pParameters = rtParams.data(),
                .NumStaticSamplers = static_cast<uint32_t>(staticSamplers.size()),
                .pStaticSamplers = staticSamplers.data(),
                .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
            },
        };

        ComPtr<ID3DBlob> blob, errorBlob;
        CHECK_HRESULT_WITH_ERROR_BLOB(D3D12SerializeVersionedRootSignature(&rtRootSigDesc, &blob, &errorBlob),
                                      errorBlob);
        CHECK_HRESULT(
            device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rtRootSig)));
    }
}

ComPtr<ID3D12StateObject> rtPso;
ComPtr<ID3D12Resource> dev_rtShaderIds;
D3D12_DISPATCH_RAYS_DESC rtDispatchDesc;

void compileShadersAndInitPipeline()
{
    // ===================================
    // RAYTRACING
    // ===================================
    {
        D3D12_DXIL_LIBRARY_DESC lib = {
            .DXILLibrary = {
                .pShaderBytecode = main_shaderBytecode,
                .BytecodeLength = std::size(main_shaderBytecode),
            },
        };

        constexpr uint32_t NUM_HIT_GROUPS = 2;
        std::array<D3D12_HIT_GROUP_DESC, NUM_HIT_GROUPS> hitGroups;
        hitGroups[HITGROUP_PRIMARY] = {
            .HitGroupExport = L"HitGroup_Primary",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .ClosestHitShaderImport = L"ClosestHit_Primary",
        };
        hitGroups[HITGROUP_LIGHTS] = {
            .HitGroupExport = L"HitGroup_Lights",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .ClosestHitShaderImport = L"ClosestHit_Lights",
        };

        D3D12_RAYTRACING_SHADER_CONFIG shaderCfg = {
            .MaxPayloadSizeInBytes = 96,
            .MaxAttributeSizeInBytes = 8,
        };

        D3D12_GLOBAL_ROOT_SIGNATURE globalSig = {
            rtRootSig.Get(),
        };

        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineCfg = {
            .MaxTraceRecursionDepth = 1,
        };

        std::vector<D3D12_STATE_SUBOBJECT> subobjects;
        {
            subobjects.push_back({ .Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, .pDesc = &lib });
            subobjects.push_back({ .Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, .pDesc = &shaderCfg });
            subobjects.push_back({ .Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, .pDesc = &globalSig });
            subobjects.push_back(
                { .Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, .pDesc = &pipelineCfg });

            for (const auto& hitGroup : hitGroups)
            {
                subobjects.push_back({ .Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, .pDesc = &hitGroup });
            }
        }

        D3D12_STATE_OBJECT_DESC desc = {
            .Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
            .NumSubobjects = static_cast<uint32_t>(subobjects.size()),
            .pSubobjects = subobjects.data(),
        };
        CHECK_HRESULT(device->CreateStateObject(&desc, IID_PPV_ARGS(&rtPso)));

        const uint32_t shaderIdsSizeBytes =
            2 * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT + NUM_HIT_GROUPS * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        dev_rtShaderIds = BufferHelper::createBasicBuffer(shaderIdsSizeBytes, &UPLOAD_HEAP);

        ComPtr<ID3D12StateObjectProperties> props;
        rtPso.As(&props);

        uint8_t* host_shaderIds;
        dev_rtShaderIds->Map(0, nullptr, reinterpret_cast<void**>(&host_shaderIds));

        auto writeShaderId = [&](const wchar_t* name, const uint32_t incrementSizeBytes)
        {
            void* id = props->GetShaderIdentifier(name);
            memcpy(host_shaderIds, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
            host_shaderIds += incrementSizeBytes;
        };

        writeShaderId(L"RayGeneration", D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        writeShaderId(L"Miss", D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        for (const auto& hitGroup : hitGroups)
        {
            writeShaderId(hitGroup.HitGroupExport, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        }

        dev_rtShaderIds->Unmap(0, nullptr);

        rtDispatchDesc = {
            .RayGenerationShaderRecord = {
                .StartAddress = dev_rtShaderIds->GetGPUVirtualAddress(),
                .SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
            },
            .MissShaderTable = {
                .StartAddress = dev_rtShaderIds->GetGPUVirtualAddress() + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
                .SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
            },
            .HitGroupTable = {
                .StartAddress = dev_rtShaderIds->GetGPUVirtualAddress() + 2 * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
                .SizeInBytes = NUM_HIT_GROUPS * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
                .StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
            },
        };
        rtDispatchDesc.Depth = 1; // z-dimension of ray dispatch (e.g. for path splitting, maybe)
    }
}

static int frameCount = 0;
static double elapsedTime = 0.0;
static auto lastTimePoint = std::chrono::high_resolution_clock::now();
static int lastFps = 0;

void updateFps(double deltaTime)
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

void captureQueuedScreenshot()
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

    D3D12_TEXTURE_COPY_LOCATION srcLocation = {
        .pResource = renderTarget.Get(),
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

    BufferHelper::stateTransitionResourceBarrier(cmdList.Get(),
                                                 renderTarget.Get(),
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&destLocation, 0, 0, 0, &srcLocation, nullptr);
    BufferHelper::stateTransitionResourceBarrier(cmdList.Get(),
                                                 renderTarget.Get(),
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void finalizeQueuedScreenshot()
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

    printf("Saved screenshot to %s\n", path.generic_string().c_str());

    screenshotRequest.readbackBuffer.Reset();
    screenshotRequest = ScreenshotRequest();
}

void render()
{
    const auto currentTimePoint = std::chrono::high_resolution_clock::now();
    const double deltaTime = std::chrono::duration<double>(currentTimePoint - lastTimePoint).count();
    lastTimePoint = currentTimePoint;

    auto& frameCtx = frameCtxs[frameCtxIdx];
    ParamBlockManager& paramBlockManager = frameCtx.paramBlockManager;

    auto& heapIndices = paramBlockManager.heapIndices;
    heapIndices->srv.instanceDatasIdx = scene.getInstanceDatasDescriptorIndex();
    heapIndices->srv.vertsIdx = scene.getVertsDescriptorIdx();
    heapIndices->srv.perTriDatasIdx = scene.getPerTriDatasDescriptorIdx();
    heapIndices->srv.areaLightsIdx = scene.getAreaLightsDescriptorIdx();

    if (!testMode)
    {
        camera.processPlayerInput(WindowManager::getPlayerInput(), deltaTime);
    }
    camera.copyParamsTo(paramBlockManager.cameraParams);

    auto& renderParams = paramBlockManager.renderParams;
    renderParams->frameNumber = frameNumber;
    renderParams->numSamplesPerPixel = SettingsManager::getAsUint("spp");
    renderParams->maxPathDepth = SettingsManager::getAsUint("maxPathDepth");
    renderParams->enableMis = SettingsManager::getAsBool("enableMis") ? 1 : 0;
    renderParams->tonemapping = SettingsManager::getAsUint("tonemapping");

    beginFrame();

    scene.update(cmdList.Get(), frameCtx.toFreeList);

    auto& sceneParams = paramBlockManager.sceneParams;
    sceneParams->numAreaLights = scene.getNumAreaLights();

    if (scene.hasTlas())
    {
        cmdList->SetPipelineState1(rtPso.Get());
        cmdList->SetComputeRootSignature(rtRootSig.Get());
        ID3D12DescriptorHeap* heaps[] = { sharedDescriptorHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);

        // clang-format off
        cmdList->SetComputeRootConstantBufferView(RT_PARAM_IDX(GLOBAL_PARAMS), paramBlockManager.getDevBuffer()->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(RT_PARAM_IDX(RAYTRACING_ACS), scene.getDevTlasAddress());
        cmdList->SetComputeRootShaderResourceView(RT_PARAM_IDX(IDXS), scene.getDevIdxsBufferAddress());
        cmdList->SetComputeRootShaderResourceView(RT_PARAM_IDX(MATERIALS), scene.getDevMaterialsAddress());
        cmdList->SetComputeRootShaderResourceView(RT_PARAM_IDX(AREA_LIGHT_SAMPLING_STRUCTURE), scene.getDevAreaLightSamplingStructureAddress());
        // clang-format on

        const auto renderTargetDesc = renderTarget->GetDesc();

        rtDispatchDesc.Width = static_cast<uint32_t>(renderTargetDesc.Width);
        rtDispatchDesc.Height = renderTargetDesc.Height;
        cmdList->DispatchRays(&rtDispatchDesc);
    }

    ComPtr<ID3D12Resource> backBuffer;
    swapChain->GetBuffer(swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));

    BufferHelper::copyResource(cmdList.Get(),
                               backBuffer.Get(),
                               D3D12_RESOURCE_STATE_PRESENT,
                               renderTarget.Get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (screenshotRequest.active)
    {
        captureQueuedScreenshot();
    }

    backBuffer.Reset();

    submitCmd();
    const uint64_t fenceValue = nextFenceValue++;
    cmdQueue->Signal(fence.Get(), fenceValue);
    frameCtx.fenceValue = fenceValue;

    swapChain->Present(1, 0);

    ++frameNumber;
    frameCtxIdx = (frameCtxIdx + 1) % NUM_FRAMES_IN_FLIGHT;

    updateFps(deltaTime);

    if (screenshotRequest.active)
    {
        finalizeQueuedScreenshot();
    }
}

void waitForFence(const uint64_t fenceValue)
{
    if (fence->GetCompletedValue() < fenceValue)
    {
        fence->SetEventOnCompletion(fenceValue, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

void beginFrame()
{
    FrameContext& frame = frameCtxs[frameCtxIdx];

    WaitForSingleObject(frameLatencyWaitable, INFINITE);
    waitForFence(frame.fenceValue);

    frame.toFreeList.freeAll();
    frame.cmdAlloc->Reset();
    cmdList->Reset(frame.cmdAlloc.Get(), nullptr);
}

void submitCmd()
{
    cmdList->Close();
    cmdQueue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(cmdList.GetAddressOf()));
}

void flush()
{
    const uint64_t fenceValue = nextFenceValue++;
    cmdQueue->Signal(fence.Get(), fenceValue);

    waitForFence(fenceValue);

    for (auto& frame : frameCtxs)
    {
        frame.fenceValue = 0;
        frame.toFreeList.freeAll();
    }
}

void destroy()
{
    flush();
    device.Reset();
}

} // namespace Renderer
