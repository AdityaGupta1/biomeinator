#include "renderer.h"

#include "dxr_common.h"

#include "param_block_manager.h"
#include "window_manager.h"
#include "buffer/acs_helper.h"
#include "buffer/buffer_helper.h"
#include "buffer/managed_buffer.h"
#include "buffer/to_free_list.h"
#include "common/common_registers.h"
#include "scene/camera.h"
#include "scene/gltf_loader.h"
#include "scene/scene.h"

#include <chrono>
#include <random>
#include <deque>
#include <filesystem>
#include <vector>
#include <cstdio>
#include <shlobj.h>

#include "stb/stb_image_write.h"

#include "shader.fxh"

using namespace DirectX;

using WindowManager::hwnd;

namespace Renderer
{

void initDevice();
void initRenderTarget();
void initCommand();
void initRootSignature();
void initPipeline();

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

void init()
{
    initDevice();
    initRenderTarget();
    initCommand();

    for (auto& frame : frameCtxs)
    {
        frame.paramBlockManager.init();
    }

    camera.init(XMConvertToRadians(defaultFovYDegrees));

    for (auto& frame : frameCtxs)
    {
        camera.copyParamsTo(frame.paramBlockManager.cameraParams);
        frame.paramBlockManager.sceneParams->frameNumber = 0;
    }

    scene.init();

    initRootSignature();
    initPipeline();
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

ComPtr<IDXGISwapChain3> swapChain;
constexpr uint32_t swapChainFlags =
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
ComPtr<ID3D12DescriptorHeap> sharedHeap;
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

    D3D12_DESCRIPTOR_HEAP_DESC sharedHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = MAX_NUM_TEXTURES + 1,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
    };
    device->CreateDescriptorHeap(&sharedHeapDesc, IID_PPV_ARGS(&sharedHeap));

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
    const uint32_t descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = { sharedHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                                                    MAX_NUM_TEXTURES * descriptorSize };
    device->CreateUnorderedAccessView(renderTarget.Get(), nullptr, &uavDesc, uavHandle);
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

enum class Param
{
    SHARED_HEAP,
    GLOBAL_PARAMS,
    RAYTRACING_ACS,
    VERTS,
    IDXS,
    INSTANCE_DATAS,
    MATERIALS,

    COUNT
};

#define PARAM_IDX(param) static_cast<uint32_t>(Param::param)

ComPtr<ID3D12RootSignature> rootSignature;
void initRootSignature()
{
    std::vector<D3D12_DESCRIPTOR_RANGE1> descriptorRanges;

    descriptorRanges.push_back({
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
        .NumDescriptors = 1,
        .BaseShaderRegister = REGISTER_RENDER_TARGET,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = MAX_NUM_TEXTURES,
    });

    descriptorRanges.push_back({
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = MAX_NUM_TEXTURES,
        .BaseShaderRegister = REGISTER_TEXTURES,
        .RegisterSpace = 0,
        .Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE,
    });

    std::array<D3D12_ROOT_PARAMETER1, PARAM_IDX(COUNT)> params;

    params[PARAM_IDX(SHARED_HEAP)] = {
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
        .DescriptorTable = {
            .NumDescriptorRanges = static_cast<uint32_t>(descriptorRanges.size()),
            .pDescriptorRanges = descriptorRanges.data(),
        },
    };

    params[PARAM_IDX(GLOBAL_PARAMS)] = {
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
        .Descriptor = {
            .ShaderRegister = REGISTER_GLOBAL_PARAMS,
            .RegisterSpace = 0,
        },
    };

    params[PARAM_IDX(RAYTRACING_ACS)] = {
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
        .Descriptor = {
            .ShaderRegister = REGISTER_RAYTRACING_ACS,
            .RegisterSpace = 0,
        },
    };

    params[PARAM_IDX(VERTS)] = {
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
        .Descriptor = {
            .ShaderRegister = REGISTER_VERTS,
            .RegisterSpace = 0,
        },
    };

    params[PARAM_IDX(IDXS)] = {
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
        .Descriptor = {
            .ShaderRegister = REGISTER_IDXS,
            .RegisterSpace = 0,
        },
    };

    params[PARAM_IDX(INSTANCE_DATAS)] = {
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
        .Descriptor = {
            .ShaderRegister = REGISTER_INSTANCE_DATAS,
            .RegisterSpace = 0,
        },
    };

    params[PARAM_IDX(MATERIALS)] = {
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
        .Descriptor = {
            .ShaderRegister = REGISTER_MATERIALS,
            .RegisterSpace = 0,
        },
    };

    std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;

    staticSamplers.push_back({
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .ShaderRegister = REGISTER_TEX_SAMPLER,
    });

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {
        .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
        .Desc_1_1 = {
            .NumParameters = static_cast<uint32_t>(params.size()),
            .pParameters = params.data(),
            .NumStaticSamplers = static_cast<uint32_t>(staticSamplers.size()),
            .pStaticSamplers = staticSamplers.data(),
            .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
        },
    };

    ComPtr<ID3DBlob> blob;
    D3D12SerializeVersionedRootSignature(&rootSigDesc, &blob, nullptr);
    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
}

ComPtr<ID3D12StateObject> pso;
constexpr UINT64 NUM_SHADER_IDS = 3;
ComPtr<ID3D12Resource> dev_shaderIds;
D3D12_DISPATCH_RAYS_DESC dispatchDesc;
void initPipeline()
{
    D3D12_DXIL_LIBRARY_DESC lib = {
        .DXILLibrary = {
            .pShaderBytecode = compiled_shader,
            .BytecodeLength = std::size(compiled_shader),
        },
    };

    D3D12_HIT_GROUP_DESC hitGroup = {
        .HitGroupExport = L"HitGroup",
        .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
        .ClosestHitShaderImport = L"ClosestHit",
    };

    D3D12_RAYTRACING_SHADER_CONFIG shaderCfg = {
        .MaxPayloadSizeInBytes = 80,
        .MaxAttributeSizeInBytes = 8,
    };

    D3D12_GLOBAL_ROOT_SIGNATURE globalSig = {
        rootSignature.Get(),
    };

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineCfg = {
        .MaxTraceRecursionDepth = 1,
    };

    D3D12_STATE_SUBOBJECT subobjects[] = {
        { .Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, .pDesc = &lib },
        { .Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, .pDesc = &hitGroup },
        { .Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, .pDesc = &shaderCfg },
        { .Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, .pDesc = &globalSig },
        { .Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, .pDesc = &pipelineCfg },
    };
    D3D12_STATE_OBJECT_DESC desc = {
        .Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
        .NumSubobjects = std::size(subobjects),
        .pSubobjects = subobjects,
    };
    device->CreateStateObject(&desc, IID_PPV_ARGS(&pso));

    dev_shaderIds = BufferHelper::createBasicBuffer(
        NUM_SHADER_IDS * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT, &UPLOAD_HEAP, D3D12_RESOURCE_STATE_GENERIC_READ);

    ComPtr<ID3D12StateObjectProperties> props;
    pso.As(&props);

    void* data;
    auto writeId = [&](const wchar_t* name)
    {
        void* id = props->GetShaderIdentifier(name);
        memcpy(data, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        data = static_cast<char*>(data) + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    };

    dev_shaderIds->Map(0, nullptr, &data);
    writeId(L"RayGeneration");
    writeId(L"Miss");
    writeId(L"HitGroup");
    dev_shaderIds->Unmap(0, nullptr);

    dispatchDesc = {
        .RayGenerationShaderRecord = {
            .StartAddress = dev_shaderIds->GetGPUVirtualAddress(),
            .SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
        },
        .MissShaderTable = {
            .StartAddress = dev_shaderIds->GetGPUVirtualAddress() + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
            .SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
        },
        .HitGroupTable = {
            .StartAddress = dev_shaderIds->GetGPUVirtualAddress() + 2 * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
            .SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
        },
    };
    dispatchDesc.Depth = 1;
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

        std::wstring title = L"Giga Minecraft - FPS: " + std::to_wstring(lastFps);
        SetWindowTextW(hwnd, title.c_str());
    }
}

void saveScreenshot()
{
    RECT rect;
    GetClientRect(hwnd, &rect);
    const uint32_t width = rect.right - rect.left;
    const uint32_t height = rect.bottom - rect.top;

    flush();

    const uint32_t rowPitchBytes = width * 4;
    const uint32_t rowPitchBytesAligned =
        (rowPitchBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const uint32_t readbackSizeBytes = rowPitchBytesAligned * height;

    ComPtr<ID3D12Resource> readbackBuffer =
        BufferHelper::createBasicBuffer(readbackSizeBytes, &READBACK_HEAP, D3D12_RESOURCE_STATE_COPY_DEST);

    beginFrame();

    D3D12_TEXTURE_COPY_LOCATION srcLocation = {
        .pResource = renderTarget.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };

    D3D12_TEXTURE_COPY_LOCATION destLocation = {};
    destLocation.pResource = readbackBuffer.Get();
    destLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destLocation.PlacedFootprint = {
        .Offset = 0,
        .Footprint = {
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .Width = width,
            .Height = height,
            .Depth = 1,
            .RowPitch = rowPitchBytesAligned,
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

    submitCmd();
    flush();

    std::vector<uint8_t> pixels(width * height * 4);
    uint8_t* mapped = nullptr;
    readbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (uint32_t row = 0; row < height; ++row)
    {
        memcpy(pixels.data() + rowPitchBytes * row, mapped + rowPitchBytesAligned * row, rowPitchBytes);
    }
    readbackBuffer->Unmap(0, nullptr);

    wchar_t docPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docPath)))
    {
        const std::filesystem::path dir =
            std::filesystem::path(docPath) / L"biomeinator" / "screenshots";
        std::filesystem::create_directories(dir);

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

        const std::filesystem::path path = dir / fileName;
        stbi_write_png(path.string().c_str(), width, height, 4, pixels.data(), width * 4);
    }
}

void render()
{
    const auto currentTimePoint = std::chrono::high_resolution_clock::now();
    const double deltaTime = std::chrono::duration<double>(currentTimePoint - lastTimePoint).count();
    lastTimePoint = currentTimePoint;

    auto& frameCtx = frameCtxs[frameCtxIdx];
    ParamBlockManager& paramBlockManager = frameCtx.paramBlockManager;

    camera.processPlayerInput(WindowManager::getPlayerInput(), deltaTime);
    camera.copyParamsTo(paramBlockManager.cameraParams);
    paramBlockManager.sceneParams->frameNumber = frameNumber;

    beginFrame();

    scene.update(cmdList.Get(), frameCtx.toFreeList);

    if (scene.getDevTlas() != nullptr)
    {
        cmdList->SetPipelineState1(pso.Get());
        cmdList->SetComputeRootSignature(rootSignature.Get());
        ID3D12DescriptorHeap* heaps[] = { sharedHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);

        // clang-format off
        cmdList->SetComputeRootDescriptorTable(PARAM_IDX(SHARED_HEAP), sharedHeap->GetGPUDescriptorHandleForHeapStart());
        cmdList->SetComputeRootConstantBufferView(PARAM_IDX(GLOBAL_PARAMS), paramBlockManager.getDevBuffer()->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(PARAM_IDX(RAYTRACING_ACS), scene.getDevTlas()->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(PARAM_IDX(VERTS), scene.getDevVertBuffer()->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(PARAM_IDX(IDXS), scene.getDevIdxBuffer()->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(PARAM_IDX(INSTANCE_DATAS), scene.getDevInstanceDatas()->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(PARAM_IDX(MATERIALS), scene.getDevMaterials()->GetGPUVirtualAddress());
        // clang-format on

        const auto renderTargetDesc = renderTarget->GetDesc();

        dispatchDesc.Width = static_cast<uint32_t>(renderTargetDesc.Width);
        dispatchDesc.Height = renderTargetDesc.Height;
        cmdList->DispatchRays(&dispatchDesc);
    }

    ComPtr<ID3D12Resource> backBuffer;
    swapChain->GetBuffer(swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));

    BufferHelper::copyResource(cmdList.Get(),
                               backBuffer.Get(),
                               D3D12_RESOURCE_STATE_PRESENT,
                               renderTarget.Get(),
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    backBuffer.Reset();

    submitCmd();
    const uint64_t fenceValue = nextFenceValue++;
    cmdQueue->Signal(fence.Get(), fenceValue);
    frameCtx.fenceValue = fenceValue;

    swapChain->Present(1, 0);

    ++frameNumber;
    frameCtxIdx = (frameCtxIdx + 1) % NUM_FRAMES_IN_FLIGHT;

    updateFps(deltaTime);
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

} // namespace Renderer
