#include "renderer.h"

#include "dxr_common.h"

#include "camera.h"
#include "scene_manager.h"
#include "window_manager.h"
#include "buffer/acs_helper.h"
#include "buffer/buffer_helper.h"
#include "buffer/managed_buffer.h"
#include "buffer/to_free_list.h"

#include <iostream>
#include <string>
#include <chrono>

#include "shader.fxh"

using namespace DirectX;

using WindowManager::hwnd;

namespace Renderer
{

void initDevice();
void initRenderTarget();
void initCommand();
void initBottomLevel();
void initScene();
void updateTransforms();
void initTopLevel();
void initRootSignature();
void initPipeline();

void resetCmd();
void submitCmd();

constexpr float fovYDegrees = 35;
Camera camera;

ToFreeList toFreeList;
ComPtr<ID3D12GraphicsCommandList4> cmdList;

void init()
{
    initDevice();
    initRenderTarget();
    initCommand();

    resetCmd();

    camera.init(XMConvertToRadians(fovYDegrees));

    SceneManager::init(cmdList.Get(), toFreeList);

    submitCmd();
    flush();

    initRootSignature();
    initPipeline();
}

ComPtr<IDXGIFactory4> factory;
ComPtr<ID3D12Device5> device;
ComPtr<ID3D12CommandQueue> cmdQueue;
ComPtr<ID3D12Fence> fence;
void initDevice()
{
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
        printf("Failed to create debug factory\n");
        CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
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
ComPtr<ID3D12DescriptorHeap> uavHeap;
void initRenderTarget()
{
    DXGI_SWAP_CHAIN_DESC1 scDesc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = NO_AA,
        .BufferCount = 2,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
    };
    ComPtr<IDXGISwapChain1> swapChain1;
    factory->CreateSwapChainForHwnd(cmdQueue.Get(), hwnd, &scDesc, nullptr, nullptr, &swapChain1);
    swapChain1.As(&swapChain);

    factory.Reset();

    D3D12_DESCRIPTOR_HEAP_DESC uavHeapDesc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 1,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
    };
    device->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&uavHeap));

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
    auto width = std::max<uint32_t>(rect.right - rect.left, 1);
    auto height = std::max<uint32_t>(rect.bottom - rect.top, 1);

    flush();

    swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);

    if (renderTarget)
    {
        renderTarget.Reset();
    }

    D3D12_RESOURCE_DESC renderTargetDesc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = NO_AA,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    };
    device->CreateCommittedResource(&DEFAULT_HEAP,
                                    D3D12_HEAP_FLAG_NONE,
                                    &renderTargetDesc,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    nullptr,
                                    IID_PPV_ARGS(&renderTarget));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D
    };
    device->CreateUnorderedAccessView(
        renderTarget.Get(), nullptr, &uavDesc, uavHeap->GetCPUDescriptorHandleForHeapStart());
}

ComPtr<ID3D12CommandAllocator> cmdAlloc;
void initCommand()
{
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&cmdList));
}


ComPtr<ID3D12RootSignature> rootSignature;
void initRootSignature()
{
    D3D12_DESCRIPTOR_RANGE1 uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;

    std::vector<D3D12_ROOT_PARAMETER1> params;

    params.push_back({ // u0
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
        .DescriptorTable = {
            .NumDescriptorRanges = 1,
            .pDescriptorRanges = &uavRange,
        },
    });

    params.push_back({ // b0
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
        .Descriptor = {
            .ShaderRegister = 0,
            .RegisterSpace = 0,
        },
    });

    params.push_back({ // t0
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
        .Descriptor = {
            .ShaderRegister = 0,
            .RegisterSpace = 0,
        },
    });

    params.push_back({ // t1
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
        .Descriptor = {
            .ShaderRegister = 1,
            .RegisterSpace = 0,
        },
    });

    params.push_back({ // t2
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
        .Descriptor = {
            .ShaderRegister = 2,
            .RegisterSpace = 0,
        },
    });

    params.push_back({ // t3
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
        .Descriptor = {
            .ShaderRegister = 3,
            .RegisterSpace = 0,
        },
    });

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSigDesc.Desc_1_1.NumParameters = params.size();
    rootSigDesc.Desc_1_1.pParameters = params.data();
    rootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    rootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

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
        .MaxPayloadSizeInBytes = 20,
        .MaxAttributeSizeInBytes = 8,
    };

    D3D12_GLOBAL_ROOT_SIGNATURE globalSig = {
        rootSignature.Get(),
    };

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineCfg = {
        .MaxTraceRecursionDepth = 3,
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

    dev_shaderIds = BufferHelper::createBasicBuffer(NUM_SHADER_IDS * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
                                                    &UPLOAD_HEAP,
                                                    D3D12_HEAP_FLAG_NONE,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ);

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
static auto lastTime = std::chrono::high_resolution_clock::now();
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

void render()
{
    auto now = std::chrono::high_resolution_clock::now();
    double deltaTime = std::chrono::duration<double>(now - lastTime).count();
    lastTime = now;

    camera.processPlayerInput(WindowManager::getPlayerInput(), deltaTime);

    resetCmd();

    SceneManager::update(cmdList.Get());

    cmdList->SetPipelineState1(pso.Get());
    cmdList->SetComputeRootSignature(rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { uavHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    auto uavTable = uavHeap->GetGPUDescriptorHandleForHeapStart();
    uint32_t paramIdx = 0;
    cmdList->SetComputeRootDescriptorTable(paramIdx++, uavTable); // u0
    cmdList->SetComputeRootConstantBufferView(paramIdx++, camera.getCameraParamsBuffer()->GetGPUVirtualAddress()); // b0
    cmdList->SetComputeRootShaderResourceView(paramIdx++, SceneManager::getDevTlas()->GetGPUVirtualAddress()); // t0
    cmdList->SetComputeRootShaderResourceView(paramIdx++, SceneManager::getDevVertBuffer()->GetGPUVirtualAddress()); // t1
    cmdList->SetComputeRootShaderResourceView(paramIdx++, SceneManager::getDevIdxBuffer()->GetGPUVirtualAddress()); // t2
    cmdList->SetComputeRootShaderResourceView(paramIdx++, SceneManager::getDevInstanceDatas()->GetGPUVirtualAddress()); // t3

    const auto renderTargetDesc = renderTarget->GetDesc();

    dispatchDesc.Width = static_cast<uint32_t>(renderTargetDesc.Width);
    dispatchDesc.Height = renderTargetDesc.Height;
    cmdList->DispatchRays(&dispatchDesc);

    ComPtr<ID3D12Resource> backBuffer;
    swapChain->GetBuffer(swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));

    BufferHelper::stateTransitionResourceBarrier(
        cmdList.Get(), renderTarget.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    BufferHelper::stateTransitionResourceBarrier(
        cmdList.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyResource(backBuffer.Get(), renderTarget.Get());

    BufferHelper::stateTransitionResourceBarrier(
        cmdList.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    BufferHelper::stateTransitionResourceBarrier(
        cmdList.Get(), renderTarget.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    backBuffer.Reset();

    submitCmd();
    flush();

    swapChain->Present(1, 0);

    toFreeList.freeAll();

    updateFps(deltaTime);
}

void resetCmd()
{
    cmdAlloc->Reset();
    cmdList->Reset(cmdAlloc.Get(), nullptr);
}

void submitCmd()
{
    cmdList->Close();
    cmdQueue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(cmdList.GetAddressOf()));
}

void flush()
{
    static uint64_t value = 1;
    uint64_t fenceValue = value++;

    cmdQueue->Signal(fence.Get(), fenceValue);

    static HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    if (fence->GetCompletedValue() < fenceValue)
    {
        fence->SetEventOnCompletion(fenceValue, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

} // namespace Renderer
