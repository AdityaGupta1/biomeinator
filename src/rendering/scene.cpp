#include "scene.h"

#include "dxr_common.h"
#include "renderer.h"
#include "buffer/acs_helper.h"
#include "buffer/buffer_helper.h"

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

Scene::Scene(uint32_t maxNumInstances) : maxNumInstances(maxNumInstances) {}

void Scene::init(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    this->dev_vertBuffer.init((quadVerts.size() + cubeVerts.size()) * sizeof(Vertex));
    this->dev_idxBuffer.init(cubeIdxs.size() * sizeof(uint32_t));

    std::vector<AcsHelper::BlasBuildInputs> allBlasInputs;

    {
        AcsHelper::BlasBuildInputs blasInputs;
        blasInputs.host_verts = &quadVerts;
        blasInputs.dev_verts = &dev_vertBuffer;
        blasInputs.outGeoWrapper = &quadGeoWrapper;
        allBlasInputs.push_back(blasInputs);
    }

    {
        AcsHelper::BlasBuildInputs blasInputs;
        blasInputs.host_verts = &cubeVerts;
        blasInputs.host_idxs = &cubeIdxs;
        blasInputs.dev_verts = &dev_vertBuffer;
        blasInputs.dev_idxs = &dev_idxBuffer;
        blasInputs.outGeoWrapper = &cubeGeoWrapper;
        allBlasInputs.push_back(blasInputs);
    }

    AcsHelper::makeBlases(cmdList, toFreeList, allBlasInputs);

    BufferHelper::uavBarrier(cmdList, nullptr);

    dev_instanceDescs = BufferHelper::createBasicBuffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * this->maxNumInstances,
                                                        &UPLOAD_HEAP,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ);
    dev_instanceDescs->Map(0, nullptr, reinterpret_cast<void**>(&this->host_instanceDescs));

    dev_instanceDatas = BufferHelper::createBasicBuffer(sizeof(InstanceData) * this->maxNumInstances,
                                                        &UPLOAD_HEAP,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ);
    dev_instanceDatas->Map(0, nullptr, reinterpret_cast<void**>(&this->host_instanceDatas));

    for (uint32_t i = 0; i < this->maxNumInstances; ++i)
    {
        const bool isQuad = i > 0;

        this->host_instanceDescs[i] = {
            .InstanceID = i,
            .InstanceMask = 1,
            .AccelerationStructure = (isQuad ? quadGeoWrapper : cubeGeoWrapper).dev_blas->GetGPUVirtualAddress(),
        };

        this->host_instanceDatas[i] = {
            .vertBufferOffset =
                (uint32_t)((isQuad ? quadGeoWrapper : cubeGeoWrapper).vertBufferSection.offsetBytes / sizeof(Vertex)),
            .hasIdxs = !isQuad,
            .idxBufferByteOffset = isQuad ? 0 : cubeGeoWrapper.idxBufferSection.offsetBytes,
        };
    }

    // TODO: remove
    {
        using namespace DirectX;
        auto set = [this](int idx, XMMATRIX mx)
        {
            auto* ptr = reinterpret_cast<XMFLOAT3X4*>(&this->host_instanceDescs[idx].Transform);
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

    makeTlas(cmdList, toFreeList);
}

void Scene::makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    AcsHelper::TlasBuildInputs inputs;
    inputs.dev_instanceDescs = dev_instanceDescs.Get();
    inputs.numInstances = this->maxNumInstances;
    inputs.outTlas = &dev_tlas;

    AcsHelper::makeTlas(cmdList, toFreeList, inputs);

    BufferHelper::uavBarrier(cmdList, dev_tlas.Get());
}

ID3D12Resource* Scene::getDevInstanceDescs()
{
    return this->dev_instanceDescs.Get();
}

ID3D12Resource* Scene::getDevInstanceDatas()
{
    return this->dev_instanceDatas.Get();
}

ID3D12Resource* Scene::getDevTlas()
{
    return this->dev_tlas.Get();
}

ID3D12Resource* Scene::getDevVertBuffer()
{
    return this->dev_vertBuffer.getBuffer();
}

ID3D12Resource* Scene::getDevIdxBuffer()
{
    return this->dev_idxBuffer.getBuffer();
}
