#include "renderer.h"

#include "dxr_common.h"

#include "acs_helper.h"
#include "camera.h"
#include "to_free_list.h"
#include "buffer/buffer_helper.h"
#include "buffer/managed_buffer.h"

#include <iostream>
#include <string>
#include <chrono>

#include "shader.fxh"

namespace Renderer
{

void initWindow();
void initDevice();
void initSurfaces(HWND hwnd);
void resize(HWND hwnd);
void initCommand();
void initBottomLevel();
void initScene();
void updateTransforms();
void initTopLevel();
void initRootSignature();
void initPipeline();

void resetCmd();
void submitCmd();
void flush();

HWND hwnd;

void init()
{
    initWindow();
    initDevice();
    initSurfaces(hwnd);
    initCommand();

    resetCmd();

    initBottomLevel();
    initScene();
    initTopLevel();

    submitCmd();
    flush();

    initRootSignature();
    initPipeline();
}

bool windowJustRegainedFocus = false;

LRESULT WINAPI onWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CLOSE:
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_SIZING:
    case WM_SIZE:
        resize(hwnd);
        break;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
        {
            flush();
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        break;
    case WM_SYSKEYDOWN: // = alt key pressed
        if (wparam == VK_F4) // allow alt + f4
        {
            break;
        }
        [[fallthrough]];
    case WM_SYSKEYUP:
    case WM_SYSCHAR: // = key pressed while alt is also pressed
        return 0;
    case WM_ACTIVATE:
        if (wparam == WA_INACTIVE)
        {
            ShowCursor(true);
        }
        else
        {
            windowJustRegainedFocus = true;
            ShowCursor(false);
        }
        break;
    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void initWindow()
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

ComPtr<ID3D12CommandAllocator> cmdAlloc;
ComPtr<ID3D12GraphicsCommandList4> cmdList;
void initCommand()
{
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&cmdList));
}

const std::vector<Vertex> quadVerts = {
    {{-1, 0, -1}, {0, 1, 0}, {0, 0}},
    {{-1, 0, 1}, {0, 1, 0}, {0, 1}},
    {{1, 0, 1}, {0, 1, 0}, {1, 1}},
    {{-1, 0, -1}, {0, 1, 0}, {0, 0}},
    {{1, 0, -1}, {0, 1, 0}, {1, 0}},
    {{1, 0, 1}, {0, 1, 0}, {1, 1}},
};
const std::vector<Vertex> cubeVerts = {
    // -x (left)
    {{-1, -1, -1}, {-1, 0, 0}, {0, 1}},
    {{-1, -1, 1}, {-1, 0, 0}, {1, 1}},
    {{-1, 1, 1}, {-1, 0, 0}, {1, 0}},
    {{-1, 1, -1}, {-1, 0, 0}, {0, 0}},

    // -y (bottom)
    {{-1, -1, -1}, {0, -1, 0}, {0, 0}},
    {{1, -1, -1}, {0, -1, 0}, {1, 0}},
    {{1, -1, 1}, {0, -1, 0}, {1, 1}},
    {{-1, -1, 1}, {0, -1, 0}, {0, 1}},

    // -z (back)
    {{-1, -1, -1}, {0, 0, -1}, {1, 1}},
    {{-1, 1, -1}, {0, 0, -1}, {1, 0}},
    {{1, 1, -1}, {0, 0, -1}, {0, 0}},
    {{1, -1, -1}, {0, 0, -1}, {0, 1}},

    // +x (right)
    {{1, -1, -1}, {1, 0, 0}, {1, 1}},
    {{1, 1, -1}, {1, 0, 0}, {1, 0}},
    {{1, 1, 1}, {1, 0, 0}, {0, 0}},
    {{1, -1, 1}, {1, 0, 0}, {0, 1}},

    // +y (top)
    {{-1, 1, -1}, {0, 1, 0}, {0, 0}},
    {{-1, 1, 1}, {0, 1, 0}, {0, 1}},
    {{1, 1, 1}, {0, 1, 0}, {1, 1}},
    {{1, 1, -1}, {0, 1, 0}, {1, 0}},

    // +z (front)
    {{-1, -1, 1}, {0, 0, 1}, {0, 1}},
    {{1, -1, 1}, {0, 0, 1}, {1, 1}},
    {{1, 1, 1}, {0, 0, 1}, {1, 0}},
    {{-1, 1, 1}, {0, 0, 1}, {0, 0}}
};
const std::vector<uint32_t> cubeIdxs = {
    // -x
    0, 1, 2, 0, 2, 3,
    // -y
    4, 5, 6, 4, 6, 7,
    // -z
    8, 9, 10, 8, 10, 11,
    // +x
    12, 13, 14, 12, 14, 15,
    // +y
    16, 17, 18, 16, 18, 19,
    // +z
    20, 21, 22, 20, 22, 23
};

ToFreeList toFreeList;

ManagedBuffer dev_vertBuffer{ &DEFAULT_HEAP, D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
ManagedBuffer dev_idxBuffer{ &DEFAULT_HEAP, D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };

AcsHelper::GeometryWrapper quadGeoWrapper;
AcsHelper::GeometryWrapper cubeGeoWrapper;

void initBottomLevel()
{
    dev_vertBuffer.init((quadVerts.size() + cubeVerts.size()) * sizeof(Vertex));
    dev_idxBuffer.init(cubeIdxs.size() * sizeof(uint32_t));

    std::vector<AcsHelper::BlasBuildInputs> allBlasInputs;

    {
        AcsHelper::BlasBuildInputs blasInputs;
        blasInputs.host_verts = &quadVerts;
        blasInputs.dev_managedVertBuffer = &dev_vertBuffer;
        blasInputs.outGeoWrapper = &quadGeoWrapper;
        allBlasInputs.push_back(blasInputs);
    }

    {
        AcsHelper::BlasBuildInputs blasInputs;
        blasInputs.host_verts = &cubeVerts;
        blasInputs.host_idxs = &cubeIdxs;
        blasInputs.dev_managedVertBuffer = &dev_vertBuffer;
        blasInputs.dev_managedIdxBuffer = &dev_idxBuffer;
        blasInputs.outGeoWrapper = &cubeGeoWrapper;
        allBlasInputs.push_back(blasInputs);
    }

    AcsHelper::makeBuffersAndBlases(cmdList.Get(), toFreeList, allBlasInputs);

    BufferHelper::uavBarrier(cmdList.Get(), nullptr);
}

constexpr float fovYDegrees = 35;
Camera camera;

constexpr uint32_t MAX_INSTANCES = 3; // will be more than NUM_INSTANCES after adding chunks
constexpr uint32_t NUM_INSTANCES = 3;
D3D12_RAYTRACING_INSTANCE_DESC* host_instanceDescs;
ComPtr<ID3D12Resource> dev_instanceDescs;
InstanceData* host_instanceDatas;
ComPtr<ID3D12Resource> dev_instanceDatas;
void initScene()
{
    camera.init(XMConvertToRadians(fovYDegrees));

    dev_instanceDescs = BufferHelper::createBasicBuffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * MAX_INSTANCES,
                                                        &UPLOAD_HEAP,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ);
    dev_instanceDescs->Map(0, nullptr, reinterpret_cast<void**>(&host_instanceDescs));

    dev_instanceDatas = BufferHelper::createBasicBuffer(
        sizeof(InstanceData) * MAX_INSTANCES, &UPLOAD_HEAP, D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    dev_instanceDatas->Map(0, nullptr, reinterpret_cast<void**>(&host_instanceDatas));

    for (uint32_t i = 0; i < NUM_INSTANCES; ++i)
    {
        const bool isQuad = i > 0;

        host_instanceDescs[i] = {
            .InstanceID = i,
            .InstanceMask = 1,
            .AccelerationStructure = (isQuad ? quadGeoWrapper : cubeGeoWrapper).dev_blas->GetGPUVirtualAddress(),
        };

        host_instanceDatas[i] = {
            .vertBufferOffset =
                (uint32_t)((isQuad ? quadGeoWrapper : cubeGeoWrapper).vertBufferSection.offsetBytes / sizeof(Vertex)),
            .hasIdx = !isQuad,
            .idxBufferByteOffset = isQuad ? 0 : cubeGeoWrapper.idxBufferSection.offsetBytes,
        };
    }
}

void updateTransforms()
{
    using namespace DirectX;
    auto set = [](int idx, XMMATRIX mx)
    {
        auto* ptr = reinterpret_cast<XMFLOAT3X4*>(&host_instanceDescs[idx].Transform);
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

ComPtr<ID3D12Resource> dev_tlas;
ComPtr<ID3D12Resource> dev_tlasUpdateScratchBuffer;
void initTopLevel()
{
    updateTransforms();

    uint64_t updateScratchSize;

    AcsHelper::TlasBuildInputs inputs;
    inputs.dev_instanceDescs = dev_instanceDescs.Get();
    inputs.numInstances = NUM_INSTANCES;
    inputs.updateScratchSizePtr = &updateScratchSize;
    inputs.outTlas = &dev_tlas;

    AcsHelper::makeTlas(cmdList.Get(), toFreeList, inputs);

    auto desc = BASIC_BUFFER_DESC;
    desc.Width = updateScratchSize;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    device->CreateCommittedResource(&DEFAULT_HEAP, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&dev_tlasUpdateScratchBuffer));
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

    D3D12_GLOBAL_ROOT_SIGNATURE globalSig = { rootSignature.Get() };

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
}

struct PlayerInput
{
    XMFLOAT3 linearInput{ 0, 0, 0 };
    float linearSpeedMultiplier{ 1.f };
    XMFLOAT2 mouseMovement{ 0, 0 };
};

PlayerInput getPlayerInput()
{
    PlayerInput input;

    if (GetForegroundWindow() != hwnd)
    {
        return input;
    }

#define KEY_DOWN(key) (GetAsyncKeyState(key) & 0x8000)

    if (KEY_DOWN('W'))
    {
        ++input.linearInput.z;
    }

    if (KEY_DOWN('A'))
    {
        --input.linearInput.x;
    }

    if (KEY_DOWN('S'))
    {
        --input.linearInput.z;
    }

    if (KEY_DOWN('D'))
    {
        ++input.linearInput.x;
    }

    if (KEY_DOWN(VK_SPACE) || KEY_DOWN('E'))
    {
        ++input.linearInput.y;
    }

    if (KEY_DOWN('Q'))
    {
        --input.linearInput.y;
    }

    if (KEY_DOWN(VK_LSHIFT))
    {
        input.linearSpeedMultiplier *= 2.f;
    }

    if (KEY_DOWN(VK_LMENU))
    {
        input.linearSpeedMultiplier *= 0.5f;
    }

#undef KEY_DOWN

    POINT cursorPos;
    GetCursorPos(&cursorPos);

    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);
    int centerX = (windowRect.left + windowRect.right) / 2;
    int centerY = (windowRect.top + windowRect.bottom) / 2;

    if (windowJustRegainedFocus)
    {
        windowJustRegainedFocus = false;
    }
    else
    {
        input.mouseMovement.x = static_cast<float>(cursorPos.x - centerX);
        input.mouseMovement.y = static_cast<float>(cursorPos.y - centerY);
    }

    SetCursorPos(centerX, centerY);

    return input;
}

constexpr float playerHorizontalSpeed = 11.0f;
constexpr float playerVerticalSpeed = 7.0f;
constexpr XMFLOAT3 playerLinearSpeed = XMFLOAT3(playerHorizontalSpeed, playerVerticalSpeed, playerHorizontalSpeed);

constexpr float mouseSensitivity = 0.0016f;

void processPlayerInput(PlayerInput input, double deltaTime)
{
    if (input.linearInput.x != 0 || input.linearInput.y != 0 || input.linearInput.z != 0)
    {
        XMVECTOR linearSpeed = XMLoadFloat3(&playerLinearSpeed);
        linearSpeed = XMVectorScale(linearSpeed, static_cast<float>(deltaTime) * input.linearSpeedMultiplier);
        const XMVECTOR linearMovement = XMVectorMultiply(linearSpeed, XMLoadFloat3(&input.linearInput));
        XMFLOAT3 storedLinearMovement;
        XMStoreFloat3(&storedLinearMovement, linearMovement);
        camera.moveLinear(storedLinearMovement);
    }

    if (input.mouseMovement.x != 0 || input.mouseMovement.y != 0)
    {
        camera.rotate(input.mouseMovement.x * mouseSensitivity, input.mouseMovement.y * mouseSensitivity);
    }
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

void updateScene()
{
    updateTransforms();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {
        .DestAccelerationStructureData = dev_tlas->GetGPUVirtualAddress(),
        .Inputs = {
            .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL,
            .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE,
            .NumDescs = NUM_INSTANCES,
            .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
            .InstanceDescs = dev_instanceDescs->GetGPUVirtualAddress(),
        },
        .SourceAccelerationStructureData = dev_tlas->GetGPUVirtualAddress(),
        .ScratchAccelerationStructureData = dev_tlasUpdateScratchBuffer->GetGPUVirtualAddress(),
    };
    cmdList->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);

    D3D12_RESOURCE_BARRIER barrier = { .Type = D3D12_RESOURCE_BARRIER_TYPE_UAV, .UAV = { .pResource = dev_tlas.Get() } };
    cmdList->ResourceBarrier(1, &barrier);
}

void render()
{
    auto now = std::chrono::high_resolution_clock::now();
    double deltaTime = std::chrono::duration<double>(now - lastTime).count();
    lastTime = now;

    const PlayerInput playerInput = getPlayerInput();
    processPlayerInput(playerInput, deltaTime);

    resetCmd();

    updateScene();

    cmdList->SetPipelineState1(pso.Get());
    cmdList->SetComputeRootSignature(rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { uavHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    auto uavTable = uavHeap->GetGPUDescriptorHandleForHeapStart();
    uint32_t paramIdx = 0;
    cmdList->SetComputeRootDescriptorTable(paramIdx++, uavTable); // u0
    cmdList->SetComputeRootConstantBufferView(paramIdx++, camera.getCameraParamsBuffer()->GetGPUVirtualAddress()); // b0
    cmdList->SetComputeRootShaderResourceView(paramIdx++, dev_tlas->GetGPUVirtualAddress()); // t0
    cmdList->SetComputeRootShaderResourceView(paramIdx++, dev_vertBuffer.getBuffer()->GetGPUVirtualAddress()); // t1
    cmdList->SetComputeRootShaderResourceView(paramIdx++, dev_idxBuffer.getBuffer()->GetGPUVirtualAddress()); // t2
    cmdList->SetComputeRootShaderResourceView(paramIdx++, dev_instanceDatas->GetGPUVirtualAddress()); // t3

    auto rtDesc = renderTarget->GetDesc();

    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {
        .RayGenerationShaderRecord = {
            .StartAddress = dev_shaderIds->GetGPUVirtualAddress(),
            .SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES
        },
        .MissShaderTable = {
            .StartAddress = dev_shaderIds->GetGPUVirtualAddress() + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
            .SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES
        },
        .HitGroupTable = {
            .StartAddress = dev_shaderIds->GetGPUVirtualAddress() + 2 * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
            .SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES
        },
        .Width = static_cast<UINT>(rtDesc.Width),
        .Height = rtDesc.Height,
        .Depth = 1
    };
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
