#include "scene.h"

#include "dxr_common.h"
#include "renderer.h"
#include "buffer/acs_helper.h"
#include "buffer/buffer_helper.h"
#include "buffer/to_free_list.h"

#include <stdexcept>

using namespace DirectX;

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

Instance::Instance(Scene* scene, uint32_t id)
    : scene(scene), id(id)
{}

void Instance::markReadyForBlasBuild()
{
    this->scene->instancesReadyForBlasBuild.push_back(this);
}

Scene::Scene(uint32_t maxNumInstances)
    : maxNumInstances(maxNumInstances)
{}

// TODO: pass in managed vert buffer and idx buffer size as params to constructor
void Scene::init(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    this->dev_vertBuffer.init((2 * quadVerts.size() + cubeVerts.size()) * sizeof(Vertex));
    this->dev_idxBuffer.init(cubeIdxs.size() * sizeof(uint32_t));

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

    for (int i = 0; i < this->maxNumInstances; ++i)
    {
        availableInstanceIds.push(i);
    }

    {
        const auto time = static_cast<float>(GetTickCount64()) / 1000;

        {
            Instance* instance = this->requestNewInstance();

            instance->host_verts = cubeVerts;
            instance->host_idxs = cubeIdxs;

            auto transform = XMMatrixRotationRollPitchYaw(time / 2, time / 3, time / 5);
            transform *= XMMatrixTranslation(-1.5, 2, 2);
            XMStoreFloat3x4(&instance->transform, transform);

            instance->markReadyForBlasBuild();
        }

        {
            Instance* instance = this->requestNewInstance();

            instance->host_verts = quadVerts;

            auto transform = XMMatrixRotationX(-1.8f);
            transform *= XMMatrixRotationY(XMScalarSinEst(time) / 8 + 1);
            transform *= XMMatrixTranslation(2, 2, 2);
            XMStoreFloat3x4(&instance->transform, transform);

            instance->markReadyForBlasBuild();
        }

        {
            Instance* instance = this->requestNewInstance();

            instance->host_verts = quadVerts;

            auto transform = XMMatrixScaling(5, 5, 5);
            transform *= XMMatrixTranslation(0, 0, 2);
            XMStoreFloat3x4(&instance->transform, transform);

            instance->markReadyForBlasBuild();
        }
    }

    this->makeQueuedBlasesAndUpdateInstances(cmdList, toFreeList);

    makeTlas(cmdList, toFreeList);
}

void Scene::update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    const bool doUpdateTlas = this->makeQueuedBlasesAndUpdateInstances(cmdList, toFreeList);

    if (doUpdateTlas)
    {
        this->makeTlas(cmdList, toFreeList);
    }
}

bool Scene::makeQueuedBlasesAndUpdateInstances(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    if (this->instancesReadyForBlasBuild.empty())
    {
        return false;
    }

    std::vector<AcsHelper::BlasBuildInputs> allBlasInputs;

    for (auto instance : instancesReadyForBlasBuild)
    {
        AcsHelper::BlasBuildInputs blasInputs;

        blasInputs.host_verts = &instance->host_verts;
        blasInputs.dev_verts = &dev_vertBuffer;

        if (instance->host_idxs.size() > 0)
        {
            blasInputs.host_idxs = &instance->host_idxs;
            blasInputs.dev_idxs = &dev_idxBuffer;
        }

        blasInputs.outGeoWrapper = &instance->geoWrapper;

        allBlasInputs.push_back(blasInputs);
    }

    AcsHelper::makeBlases(cmdList, toFreeList, allBlasInputs);

    BufferHelper::uavBarrier(cmdList, nullptr);

    for (const auto instance : this->instancesReadyForBlasBuild)
    {
        D3D12_RAYTRACING_INSTANCE_DESC& instanceDesc = this->host_instanceDescs[instance->id];
        memcpy(instanceDesc.Transform, &instance->transform, sizeof(XMFLOAT3X4));
        instanceDesc.InstanceID = instance->id;
        instanceDesc.InstanceMask = 1;
        instanceDesc.AccelerationStructure = instance->geoWrapper.dev_blas->GetGPUVirtualAddress();

        InstanceData& data = this->host_instanceDatas[instance->id];
        data.vertBufferOffset =
            instance->geoWrapper.vertBufferSection.offsetBytes / static_cast<uint32_t>(sizeof(Vertex));
        data.hasIdxs = instance->geoWrapper.idxBufferSection.sizeBytes > 0;
        data.idxBufferByteOffset = instance->geoWrapper.idxBufferSection.offsetBytes;
    }

    this->instancesReadyForBlasBuild.clear();
    return true;
}

void Scene::makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    if (this->dev_tlas)
    {
        toFreeList.pushResource(this->dev_tlas);
    }

    AcsHelper::TlasBuildInputs inputs;
    inputs.dev_instanceDescs = this->dev_instanceDescs.Get();
    inputs.numInstances = this->maxNumInstances;
    inputs.outTlas = &this->dev_tlas;

    AcsHelper::makeTlas(cmdList, toFreeList, inputs);

    BufferHelper::uavBarrier(cmdList, this->dev_tlas.Get());
}

Instance* Scene::requestNewInstance()
{
    if (this->availableInstanceIds.empty())
    {
        throw std::runtime_error("Scene has no available instance ids");
    }

    const uint32_t id = this->availableInstanceIds.front();
    this->availableInstanceIds.pop();

    std::unique_ptr<Instance> newInstance = std::unique_ptr<Instance>(new Instance(this, id));
    Instance* newInstancePtr = newInstance.get();
    this->instances.emplace(id, std::move(newInstance));

    return newInstancePtr;
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
    return this->dev_vertBuffer.getManagedBuffer();
}

ID3D12Resource* Scene::getDevIdxBuffer()
{
    return this->dev_idxBuffer.getManagedBuffer();
}
