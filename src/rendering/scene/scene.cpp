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

void Scene::clear()
{
    this->instances.clear();
    this->instancesReadyForBlasBuild.clear();
    this->availableInstanceIds = {};
    for (uint32_t instanceIdx = 0; instanceIdx < this->maxNumInstances; ++instanceIdx)
    {
        this->availableInstanceIds.push(instanceIdx);
    }

    this->nextMaterialId = 0;

    this->isTlasDirty = false;
    this->dev_tlas = nullptr;

    this->textures.fill(nullptr);
    this->nextTextureId = 0;
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

uint32_t Scene::addTexture(std::vector<uint8_t>&& data, uint32_t width, uint32_t height)
{
    if (this->nextTextureId >= MAX_NUM_TEXTURES)
    {
        throw std::runtime_error("Scene out of texture slots");
    }

    const uint32_t id = this->nextTextureId++;
    this->pendingTextures.push_back({ std::move(data), width, height, id });
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
    this->uploadPendingTextures(cmdList, toFreeList);
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

void Scene::uploadPendingTextures(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    if (this->pendingTextures.empty())
    {
        return;
    }

    const uint32_t descriptorSize =
        Renderer::device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE heapCpuHandle =
        Renderer::sharedHeap->GetCPUDescriptorHandleForHeapStart();

    for (const auto& pendingTex : this->pendingTextures)
    {
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = pendingTex.width;
        texDesc.Height = pendingTex.height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        texDesc.SampleDesc = NO_AA;

        ComPtr<ID3D12Resource> dev_texture;
        CHECK_HRESULT(Renderer::device->CreateCommittedResource(&DEFAULT_HEAP,
                                                                D3D12_HEAP_FLAG_NONE,
                                                                &texDesc,
                                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                                nullptr,
                                                                IID_PPV_ARGS(&dev_texture)));

        const uint32_t rowPitchBytes = pendingTex.width * 4;
        const uint32_t rowPitchAligned =
            (rowPitchBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        const uint32_t uploadSizeBytes = rowPitchAligned * pendingTex.height;

        ComPtr<ID3D12Resource> dev_uploadBuffer =
            BufferHelper::createBasicBuffer(uploadSizeBytes, &UPLOAD_HEAP, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* host_uploadBuffer = nullptr;
        dev_uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&host_uploadBuffer));

        for (uint32_t row = 0; row < pendingTex.height; ++row)
        {
            const uint8_t* srcPtr = pendingTex.data.data() + rowPitchBytes * row;
            uint8_t* dstPtr = host_uploadBuffer + rowPitchAligned * row;
            memcpy(dstPtr, srcPtr, rowPitchBytes);
        }

        D3D12_SUBRESOURCE_FOOTPRINT footprint = {};
        footprint.Format = texDesc.Format;
        footprint.Width = pendingTex.width;
        footprint.Height = pendingTex.height;
        footprint.Depth = 1;
        footprint.RowPitch = rowPitchAligned;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = { 0, footprint };

        D3D12_TEXTURE_COPY_LOCATION src = { dev_uploadBuffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        src.PlacedFootprint = layout;
        D3D12_TEXTURE_COPY_LOCATION dst = { dev_texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        dst.SubresourceIndex = 0;

        BufferHelper::stateTransitionResourceBarrier(
            cmdList, dev_texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        BufferHelper::stateTransitionResourceBarrier(
            cmdList, dev_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        const D3D12_CPU_DESCRIPTOR_HANDLE handle = { heapCpuHandle.ptr + descriptorSize * pendingTex.id };
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        Renderer::device->CreateShaderResourceView(dev_texture.Get(), &srvDesc, handle);

        this->textures[pendingTex.id] = dev_texture;
        toFreeList.pushResource(dev_uploadBuffer, true);
    }

    this->pendingTextures.clear();
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
