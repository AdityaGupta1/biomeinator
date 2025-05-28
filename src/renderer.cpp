#include "renderer.h"

#include "common_structs.h"

#include <iostream>
#include <string>
#include <chrono>

#include "shader.fxh"

namespace Renderer
{

constexpr DXGI_SAMPLE_DESC NO_AA = {
.Count = 1,
.Quality = 0
};
constexpr D3D12_HEAP_PROPERTIES UPLOAD_HEAP = {
    .Type = D3D12_HEAP_TYPE_UPLOAD
};
constexpr D3D12_HEAP_PROPERTIES DEFAULT_HEAP = {
    .Type = D3D12_HEAP_TYPE_DEFAULT
};
constexpr D3D12_RESOURCE_DESC BASIC_BUFFER_DESC = {
    .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Width = 0, // will be changed in copies
    .Height = 1,
    .DepthOrArraySize = 1,
    .MipLevels = 1,
    .SampleDesc = NO_AA,
    .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags = D3D12_RESOURCE_FLAG_NONE,
};

LRESULT WINAPI onWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
        case WM_CLOSE:
        case WM_DESTROY:
            PostQuitMessage(0);
            [[fallthrough]];
        case WM_SIZING:
        case WM_SIZE:
            resize(hwnd);
            [[fallthrough]];
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE)
            {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            break;
        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

HWND hwnd;

void init()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wcw = {
        .lpfnWndProc = &onWindowMessage,
        .hCursor = LoadCursor(nullptr, IDC_ARROW),
        .lpszClassName = L"GigaMinecraftClass"
    };
    RegisterClassW(&wcw);

    hwnd = CreateWindowExW(0, L"GigaMinecraftClass", L"Giga Minecraft",
        WS_VISIBLE | WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        /*width=*/CW_USEDEFAULT, /*height=*/CW_USEDEFAULT,
        nullptr, nullptr, nullptr, nullptr);

    initDevice();
    initSurfaces(hwnd);
    initCommand();
    initMeshes();
    initBottomLevel();
    initScene();
    initTopLevel();
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
        printf("enabled debug layer\n");
        debug->EnableDebugLayer();
    }

    if (SUCCEEDED(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory))))
    {
        printf("created debug factory\n");
    }
    else
    {
        printf("failed to create debug factory\n");
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
            printf("selected adapter: %ls\n", desc.Description);
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

// TODO: check if this actually blocks the CPU until the GPU finishes its commands
void flush()
{
    static UINT64 value = 1;
    cmdQueue->Signal(fence.Get(), value);
    fence->SetEventOnCompletion(value++, nullptr);
}

ComPtr<IDXGISwapChain3> swapChain;
ComPtr<ID3D12DescriptorHeap> uavHeap;
void initSurfaces(HWND hwnd)
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

    resize(hwnd);
}

ComPtr<ID3D12Resource> renderTarget;
void resize(HWND hwnd)
{
    if (!swapChain) [[unlikely]]
    {
        return;
    }

    RECT rect;
    GetClientRect(hwnd, &rect);
    auto width = std::max<UINT>(rect.right - rect.left, 1);
    auto height = std::max<UINT>(rect.bottom - rect.top, 1);

    flush();

    swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);

    if (renderTarget) [[likely]]
    {
        renderTarget.Reset();
    }

    D3D12_RESOURCE_DESC rtDesc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = NO_AA,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    };
    device->CreateCommittedResource(&DEFAULT_HEAP, D3D12_HEAP_FLAG_NONE, &rtDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&renderTarget));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D
    };
    device->CreateUnorderedAccessView(renderTarget.Get(), nullptr, &uavDesc, uavHeap->GetCPUDescriptorHandleForHeapStart());
}

ID3D12CommandAllocator* cmdAlloc;
ID3D12GraphicsCommandList4* cmdList;
void initCommand()
{
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&cmdList));
}

constexpr Vertex quadVtx[] = {
    { float3(-1, 0, -1), float3(0, 1, 0), float2(0, 0) },
    { float3(-1, 0, 1), float3(0, 1, 0), float2(0, 1) },
    { float3(1, 0, 1), float3(0, 1, 0), float2(1, 1) },
    { float3(-1, 0, -1), float3(0, 1, 0), float2(0, 0) },
    { float3(1, 0, -1), float3(0, 1, 0), float2(1, 0) },
    { float3(1, 0, 1), float3(0, 1, 0), float2(1, 1) }
};
constexpr Vertex cubeVtx[] = {
    // +y (top)
    {{-1, 1, -1}, {0, 1, 0}, {0, 0}},
    {{-1, 1, 1}, {0, 1, 0}, {0, 1}},
    {{1, 1, 1}, {0, 1, 0}, {1, 1}},
    {{1, 1, -1}, {0, 1, 0}, {1, 0}},

    // -y (bottom)
    {{-1, -1, -1}, {0, -1, 0}, {0, 0}},
    {{1, -1, -1}, {0, -1, 0}, {1, 0}},
    {{1, -1, 1}, {0, -1, 0}, {1, 1}},
    {{-1, -1, 1}, {0, -1, 0}, {0, 1}},

    // +z (front)
    {{-1, -1, 1}, {0, 0, 1}, {0, 1}},
    {{1, -1, 1}, {0, 0, 1}, {1, 1}},
    {{1, 1, 1}, {0, 0, 1}, {1, 0}},
    {{-1, 1, 1}, {0, 0, 1}, {0, 0}},

    // -z (back)
    {{-1, -1, -1}, {0, 0, -1}, {1, 1}},
    {{-1, 1, -1}, {0, 0, -1}, {1, 0}},
    {{1, 1, -1}, {0, 0, -1}, {0, 0}},
    {{1, -1, -1}, {0, 0, -1}, {0, 1}},

    // -x (left)
    {{-1, -1, -1}, {-1, 0, 0}, {0, 1}},
    {{-1, -1, 1}, {-1, 0, 0}, {1, 1}},
    {{-1, 1, 1}, {-1, 0, 0}, {1, 0}},
    {{-1, 1, -1}, {-1, 0, 0}, {0, 0}},

    // +x (right)
    {{1, -1, -1}, {1, 0, 0}, {1, 1}},
    {{1, 1, -1}, {1, 0, 0}, {1, 0}},
    {{1, 1, 1}, {1, 0, 0}, {0, 0}},
    {{1, -1, 1}, {1, 0, 0}, {0, 1}}
};
constexpr short cubeIdx[] = {
    // +y
    0, 1, 2, 0, 2, 3,
    // -y
    4, 5, 6, 4, 6, 7,
    // +z
    8, 9, 10, 8, 10, 11,
    // -z
    12, 13, 14, 12, 14, 15,
    // -x
    16, 17, 18, 16, 18, 19,
    // +x
    20, 21, 22, 20, 22, 23
};

ComPtr<ID3D12Resource> quadVB;
ComPtr<ID3D12Resource> cubeVB;
ComPtr<ID3D12Resource> cubeIB;
void initMeshes()
{
    auto makeAndCopy = [](auto& data) -> ComPtr<ID3D12Resource>
    {
        auto desc = BASIC_BUFFER_DESC;
        desc.Width = sizeof(data);

        ComPtr<ID3D12Resource> res;
        device->CreateCommittedResource(&UPLOAD_HEAP, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&res));

        void* ptr;
        res->Map(0, nullptr, &ptr);
        memcpy(ptr, data, sizeof(data));
        res->Unmap(0, nullptr);

        return res;
    };

    quadVB = makeAndCopy(quadVtx);
    cubeVB = makeAndCopy(cubeVtx);
    cubeIB = makeAndCopy(cubeIdx);
}

ComPtr<ID3D12Resource> makeAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, UINT64* updateScratchSize = nullptr)
{
    auto makeBuffer = [](UINT64 size, auto initialState)
    {
        auto desc = BASIC_BUFFER_DESC;
        desc.Width = size;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> buffer;
        device->CreateCommittedResource(&DEFAULT_HEAP, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&buffer));
        return buffer;
    };

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo;
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);
    if (updateScratchSize)
    {
        *updateScratchSize = prebuildInfo.UpdateScratchDataSizeInBytes;
    }

    auto scratch = makeBuffer(prebuildInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON);
    auto accelerationStructure = makeBuffer(prebuildInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {
        .DestAccelerationStructureData = accelerationStructure->GetGPUVirtualAddress(),
        .Inputs = inputs,
        .ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress()
    };

    cmdAlloc->Reset();
    cmdList->Reset(cmdAlloc, nullptr);
    cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    cmdList->Close();
    cmdQueue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&cmdList));

    flush();
    return accelerationStructure;
}

ComPtr<ID3D12Resource> makeBLAS(ID3D12Resource* vertexBuffer, UINT numVerts, ID3D12Resource* indexBuffer = nullptr, UINT numIdx = 0)
{
    D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {
        .Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
        .Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE,

        .Triangles = {
            .Transform3x4 = 0,
            .IndexFormat = indexBuffer ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_UNKNOWN,
            .VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT,
            .IndexCount = numIdx,
            .VertexCount = numVerts,
            .IndexBuffer = indexBuffer ? indexBuffer->GetGPUVirtualAddress() : 0,
            .VertexBuffer = {
                .StartAddress = vertexBuffer->GetGPUVirtualAddress(),
                .StrideInBytes = sizeof(Vertex)
            }
        }
    };

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
        .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
        .NumDescs = 1,
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
        .pGeometryDescs = &geometryDesc
    };

    return makeAccelerationStructure(inputs);
}

ComPtr<ID3D12Resource> quadBlas;
ComPtr<ID3D12Resource> cubeBlas;
void initBottomLevel()
{
    quadBlas = makeBLAS(quadVB.Get(), std::size(quadVtx));
    cubeBlas = makeBLAS(cubeVB.Get(), std::size(cubeVtx), cubeIB.Get(), std::size(cubeIdx));
}

ComPtr<ID3D12Resource> makeTLAS(ID3D12Resource* instances, UINT numInstances, UINT64* updateScratchSize)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
        .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE,
        .NumDescs = numInstances,
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
        .InstanceDescs = instances->GetGPUVirtualAddress()
    };

    return makeAccelerationStructure(inputs, updateScratchSize);
}

constexpr UINT NUM_INSTANCES = 3;
ComPtr<ID3D12Resource> instances;
D3D12_RAYTRACING_INSTANCE_DESC* instanceData;
void initScene()
{
    auto instancesDesc = BASIC_BUFFER_DESC;
    instancesDesc.Width = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * NUM_INSTANCES;
    device->CreateCommittedResource(&UPLOAD_HEAP, D3D12_HEAP_FLAG_NONE,
        &instancesDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&instances));
    instances->Map(0, nullptr, reinterpret_cast<void**>(&instanceData));

    for (UINT i = 0; i < NUM_INSTANCES; ++i)
    {
        instanceData[i] = {
            .InstanceID = i,
            .InstanceMask = 1,
            .AccelerationStructure = (i ? quadBlas : cubeBlas)->GetGPUVirtualAddress(),
        };
    }

    updateTransforms();
}

void updateTransforms()
{
    using namespace DirectX;
    auto set = [](int idx, XMMATRIX mx)
    {
        auto* ptr = reinterpret_cast<XMFLOAT3X4*>(&instanceData[idx].Transform);
        XMStoreFloat3x4(ptr, mx);
    };

    auto time = static_cast<float>(GetTickCount64()) / 1000;

    auto cube = XMMatrixRotationRollPitchYaw(time / 2, time / 3, time / 5);
    cube *= XMMatrixTranslation(-1.5, 2, 2);
    set(0, cube);

    auto mirror = XMMatrixRotationX(-1.8f);
    mirror *= XMMatrixRotationY(XMScalarSinEst(time) / 8 + 1);
    mirror *= XMMatrixTranslation(2, 2, 2);
    set(1, mirror);

    auto floor = XMMatrixScaling(5, 5, 5);
    floor *= XMMatrixTranslation(0, 0, 2);
    set(2, floor);
}

ComPtr<ID3D12Resource> tlas;
ComPtr<ID3D12Resource> tlasUpdateScratch;
void initTopLevel()
{
    UINT64 updateScratchSize;
    tlas = makeTLAS(instances.Get(), NUM_INSTANCES, &updateScratchSize);

    auto desc = BASIC_BUFFER_DESC;
    // WARP bug workaround: use 8 if the required size was reported as less
    desc.Width = std::max(updateScratchSize, 8ULL);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    device->CreateCommittedResource(&DEFAULT_HEAP, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&tlasUpdateScratch));
}

ComPtr<ID3D12RootSignature> rootSignature;
void initRootSignature()
{
    D3D12_DESCRIPTOR_RANGE uavRange = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
        .NumDescriptors = 1,
    };
    D3D12_ROOT_PARAMETER params[] = {
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = {
                .NumDescriptorRanges = 1,
                .pDescriptorRanges = &uavRange
            }
        },
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
            .Descriptor = {
                .ShaderRegister = 0,
                .RegisterSpace = 0
            }
        }
    };

    D3D12_ROOT_SIGNATURE_DESC desc = {
        .NumParameters = std::size(params),
        .pParameters = params
    };

    ComPtr<ID3DBlob> blob;
    D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, nullptr);
    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
}

ComPtr<ID3D12StateObject> pso;
constexpr UINT64 NUM_SHADER_IDS = 3;
ComPtr<ID3D12Resource> shaderIDs;
void initPipeline()
{
    D3D12_DXIL_LIBRARY_DESC lib = {
        .DXILLibrary = {
            .pShaderBytecode = compiled_shader,
            .BytecodeLength = std::size(compiled_shader)
        }
    };

    D3D12_HIT_GROUP_DESC hitGroup = {
        .HitGroupExport = L"HitGroup",
        .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
        .ClosestHitShaderImport = L"ClosestHit"
    };

    D3D12_RAYTRACING_SHADER_CONFIG shaderCfg = {
        .MaxPayloadSizeInBytes = 20,
        .MaxAttributeSizeInBytes = 8,
    };

    D3D12_GLOBAL_ROOT_SIGNATURE globalSig = { rootSignature.Get() };

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineCfg = {
        .MaxTraceRecursionDepth = 3
    };

    D3D12_STATE_SUBOBJECT subobjects[] = {
        {.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, .pDesc = &lib},
        {.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, .pDesc = &hitGroup},
        {.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, .pDesc = &shaderCfg},
        {.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, .pDesc = &globalSig},
        {.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, .pDesc = &pipelineCfg} };
    D3D12_STATE_OBJECT_DESC desc = {
        .Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
        .NumSubobjects = std::size(subobjects),
        .pSubobjects = subobjects
    };
    device->CreateStateObject(&desc, IID_PPV_ARGS(&pso));

    auto idDesc = BASIC_BUFFER_DESC;
    idDesc.Width = NUM_SHADER_IDS * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    device->CreateCommittedResource(&UPLOAD_HEAP, D3D12_HEAP_FLAG_NONE, &idDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&shaderIDs));

    ComPtr<ID3D12StateObjectProperties> props;
    pso.As(&props);

    void* data;
    auto writeId = [&](const wchar_t* name)
    {
        void* id = props->GetShaderIdentifier(name);
        memcpy(data, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        data = static_cast<char*>(data) + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    };

    shaderIDs->Map(0, nullptr, &data);
    writeId(L"RayGeneration");
    writeId(L"Miss");
    writeId(L"HitGroup");
    shaderIDs->Unmap(0, nullptr);
}

void updateScene()
{
    updateTransforms();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {
        .DestAccelerationStructureData = tlas->GetGPUVirtualAddress(),
        .Inputs = {
            .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
            .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE,
            .NumDescs = NUM_INSTANCES,
            .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
            .InstanceDescs = instances->GetGPUVirtualAddress()
        },
        .SourceAccelerationStructureData = tlas->GetGPUVirtualAddress(),
        .ScratchAccelerationStructureData = tlasUpdateScratch->GetGPUVirtualAddress(),
    };
    cmdList->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
        .UAV = {.pResource = tlas.Get() }
    };
    cmdList->ResourceBarrier(1, &barrier);
}

static int frameCount = 0;
static double elapsedTime = 0.0;
static auto lastTime = std::chrono::high_resolution_clock::now();
static int lastFps = 0;

void updateFps()
{
    frameCount++;
    auto now = std::chrono::high_resolution_clock::now();
    double delta = std::chrono::duration<double>(now - lastTime).count();
    elapsedTime += delta;
    lastTime = now;

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
    cmdAlloc->Reset();
    cmdList->Reset(cmdAlloc, nullptr);

    updateScene();

    cmdList->SetPipelineState1(pso.Get());
    cmdList->SetComputeRootSignature(rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { uavHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    auto uavTable = uavHeap->GetGPUDescriptorHandleForHeapStart();
    cmdList->SetComputeRootDescriptorTable(0, uavTable); // u0
    cmdList->SetComputeRootShaderResourceView(1, tlas->GetGPUVirtualAddress()); // t0

    auto rtDesc = renderTarget->GetDesc();

    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {
        .RayGenerationShaderRecord = {
            .StartAddress = shaderIDs->GetGPUVirtualAddress(),
            .SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES
        },
        .MissShaderTable = {
            .StartAddress = shaderIDs->GetGPUVirtualAddress() + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
            .SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES
        },
        .HitGroupTable = {
            .StartAddress = shaderIDs->GetGPUVirtualAddress() + 2 * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
            .SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES
        },
        .Width = static_cast<UINT>(rtDesc.Width),
        .Height = rtDesc.Height,
        .Depth = 1
    };
    cmdList->DispatchRays(&dispatchDesc);

    ComPtr<ID3D12Resource> backBuffer;
    swapChain->GetBuffer(swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));

    auto barrier = [](auto* resource, auto before, auto after)
    {
        D3D12_RESOURCE_BARRIER rb = {
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = {
                .pResource = resource,
                .StateBefore = before,
                .StateAfter = after
            },
        };
        cmdList->ResourceBarrier(1, &rb);
    };

    barrier(renderTarget.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyResource(backBuffer.Get(), renderTarget.Get());

    barrier(backBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    barrier(renderTarget.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    backBuffer.Reset();

    cmdList->Close();
    cmdQueue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&cmdList));

    flush();
    swapChain->Present(1, 0);

    updateFps();
}

} // namespace Renderer
