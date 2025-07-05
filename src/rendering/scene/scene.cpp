#include "scene.h"

#include "rendering/buffer/acs_helper.h"
#include "rendering/buffer/buffer_helper.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/dxr_common.h"
#include "rendering/renderer.h"

#include <stdexcept>

using namespace DirectX;

Instance::Instance(Scene* scene, uint32_t id)
    : scene(scene), id(id)
{}

void Instance::setMaterialId(uint32_t id)
{
    this->materialId = id;
}

void Scene::init()
{
    // these resources can be dynamically resized later
    this->maxNumInstances = 1;
    this->maxNumMaterials = 1;
    this->dev_vertBuffer.init(512 /*bytes*/);
    this->dev_idxBuffer.init(128 /*bytes*/);

    this->initInstanceBuffers();
    this->initMaterialBuffers();

    for (int instanceIdx = 0; instanceIdx < this->maxNumInstances; ++instanceIdx)
    {
        availableInstanceIds.push(instanceIdx);
    }
}

void Scene::initInstanceBuffers()
{
    dev_instanceDescs = BufferHelper::createBasicBuffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * this->maxNumInstances,
                                                        &UPLOAD_HEAP,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ);
    dev_instanceDescs->Map(0, nullptr, reinterpret_cast<void**>(&this->host_instanceDescs));

    dev_instanceDatas = BufferHelper::createBasicBuffer(
        sizeof(InstanceData) * this->maxNumInstances, &UPLOAD_HEAP, D3D12_RESOURCE_STATE_GENERIC_READ);
    dev_instanceDatas->Map(0, nullptr, reinterpret_cast<void**>(&this->host_instanceDatas));
}

Instance* Scene::requestNewInstance(ToFreeList& toFreeList)
{
    if (this->availableInstanceIds.empty())
    {
        this->resizeInstanceBuffers(toFreeList, this->maxNumInstances * 2);
    }

    const uint32_t id = this->availableInstanceIds.front();
    this->availableInstanceIds.pop();

    // can't use make_unique() here since the constructor is private and accessed through friend relationship
    std::unique_ptr<Instance> newInstance = std::unique_ptr<Instance>(new Instance(this, id));
    Instance* newInstancePtr = newInstance.get();
    this->instances.emplace(id, std::move(newInstance));

    return newInstancePtr;
}

void Scene::markInstanceReadyForBlasBuild(Instance* instance)
{
    this->instancesReadyForBlasBuild.push_back(instance);
}

void Scene::resizeInstanceBuffers(ToFreeList& toFreeList, uint32_t newMaxNumInstances)
{
    toFreeList.pushResource(this->dev_instanceDatas, true);
    toFreeList.pushResource(this->dev_instanceDescs, true);

    const uint32_t oldMaxNumInstances = this->maxNumInstances;
    this->maxNumInstances = newMaxNumInstances;

    const D3D12_RAYTRACING_INSTANCE_DESC* host_oldInstanceDescs = this->host_instanceDescs;
    const InstanceData* host_oldInstanceDatas = this->host_instanceDatas;

    this->initInstanceBuffers();

    memcpy(
        this->host_instanceDescs, host_oldInstanceDescs, oldMaxNumInstances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    memcpy(this->host_instanceDatas, host_oldInstanceDatas, oldMaxNumInstances * sizeof(InstanceData));

    for (int instanceIdx = oldMaxNumInstances; instanceIdx < this->maxNumInstances; ++instanceIdx)
    {
        this->availableInstanceIds.push(instanceIdx);
    }
}

void Scene::freeInstance(Instance* instance)
{
    this->availableInstanceIds.push(instance->id);
    this->instances.erase(instance->id);
}

void Scene::initMaterialBuffers()
{
    dev_materials = BufferHelper::createBasicBuffer(
        sizeof(Material) * this->maxNumMaterials, &UPLOAD_HEAP, D3D12_RESOURCE_STATE_GENERIC_READ);
    dev_materials->Map(0, nullptr, reinterpret_cast<void**>(&this->host_materials));
}

uint32_t Scene::addMaterial(ToFreeList& toFreeList, const Material* material)
{
    if (this->nextMaterialId >= this->maxNumMaterials)
    {
        this->resizeMaterialBuffers(toFreeList, this->maxNumMaterials * 2);
    }

    const uint32_t id = this->nextMaterialId++;
    this->host_materials[id] = *material;

    return id;
}

void Scene::resizeMaterialBuffers(ToFreeList& toFreeList, uint32_t newNumMaterials)
{
    toFreeList.pushResource(this->dev_materials, true);

    const uint32_t oldMaxNumMaterials = this->maxNumMaterials;
    this->maxNumMaterials = newNumMaterials;

    const Material* host_oldMaterials = this->host_materials;

    this->initMaterialBuffers();

    memcpy(this->host_materials, host_oldMaterials, oldMaxNumMaterials * sizeof(Material));
}

void Scene::update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    this->isTlasDirty |= this->makeQueuedBlases(cmdList, toFreeList);

    if (this->isTlasDirty)
    {
        this->makeTlas(cmdList, toFreeList);
    }
}

bool Scene::makeQueuedBlases(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    if (this->instancesReadyForBlasBuild.empty())
    {
        return false;
    }

    std::vector<AcsHelper::BlasBuildInputs> allBlasInputs;

    for (const auto instance : instancesReadyForBlasBuild)
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
        InstanceData& data = this->host_instanceDatas[instance->id];
        data.vertBufferOffset =
            instance->geoWrapper.vertBufferSection.offsetBytes / static_cast<uint32_t>(sizeof(Vertex));
        data.hasIdxs = instance->geoWrapper.idxBufferSection.sizeBytes > 0;
        data.idxBufferByteOffset = instance->geoWrapper.idxBufferSection.offsetBytes;
        data.materialId = instance->materialId;

        instance->host_verts.clear();
        instance->host_idxs.clear();
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

    uint32_t instanceDescIdx = 0;
    for (const auto& [instanceId, instance] : this->instances)
    {
        if (instance->isScheduledForDeletion)
        {
            continue;
        }

        D3D12_RAYTRACING_INSTANCE_DESC& instanceDesc = this->host_instanceDescs[instanceDescIdx++];
        memcpy(instanceDesc.Transform, &instance->transform, sizeof(XMFLOAT3X4));
        instanceDesc.InstanceID = instanceId;
        instanceDesc.InstanceMask = 1;
        instanceDesc.AccelerationStructure = instance->geoWrapper.dev_blas->GetGPUVirtualAddress();
    }

    AcsHelper::TlasBuildInputs inputs;
    inputs.dev_instanceDescs = this->dev_instanceDescs.Get();
    inputs.numInstances = instanceDescIdx;
    inputs.outTlas = &this->dev_tlas;

    AcsHelper::makeTlas(cmdList, toFreeList, inputs);
    this->isTlasDirty = false;

    BufferHelper::uavBarrier(cmdList, this->dev_tlas.Get());
}

ID3D12Resource* Scene::getDevInstanceDescs()
{
    return this->dev_instanceDescs.Get();
}

ID3D12Resource* Scene::getDevInstanceDatas()
{
    return this->dev_instanceDatas.Get();
}

ID3D12Resource* Scene::getDevMaterials()
{
    return this->dev_materials.Get();
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
