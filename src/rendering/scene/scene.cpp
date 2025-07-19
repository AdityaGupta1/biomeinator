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
    this->managedVertsBuffer.init(512 /*bytes*/);
    this->managedIdxsBuffer.init(128 /*bytes*/);

    this->maxNumInstances = 1;
    this->mappedInstanceDescsArray.init(this->maxNumInstances);
    this->mappedInstanceDatasArray.init(this->maxNumInstances);
    for (int instanceIdx = 0; instanceIdx < this->maxNumInstances; ++instanceIdx)
    {
        availableInstanceIds.push(instanceIdx);
    }

    this->mappedMaterialsArray.init(1);

    this->managedAreaLightsBuffer.init(512 /*bytes*/);
    this->areaLightSamplingStructure.init(1);
}

void Scene::clear()
{
    this->managedVertsBuffer.freeAll();
    this->managedIdxsBuffer.freeAll();

    this->instances.clear();
    this->instancesReadyForBlasBuild.clear();
    this->availableInstanceIds = {};
    for (uint32_t instanceIdx = 0; instanceIdx < this->maxNumInstances; ++instanceIdx)
    {
        this->availableInstanceIds.push(instanceIdx);
    }

    this->nextMaterialIdx = 0;

    this->isTlasDirty = false;
    this->dev_tlas = nullptr;

    this->textures.fill(nullptr);
    this->nextTextureId = 0;
    this->pendingTextures.clear();

    this->numAreaLights = 0;
}

Instance* Scene::requestNewInstance(ToFreeList& toFreeList)
{
    if (this->availableInstanceIds.empty())
    {
        const uint32_t oldMaxNumInstances = this->maxNumInstances;

        this->maxNumInstances *= 2;
        this->mappedInstanceDescsArray.resize(toFreeList, this->maxNumInstances);
        this->mappedInstanceDatasArray.resize(toFreeList, this->maxNumInstances);

        for (int instanceIdx = oldMaxNumInstances; instanceIdx < this->maxNumInstances; ++instanceIdx)
        {
            this->availableInstanceIds.push(instanceIdx);
        }
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

void Scene::freeInstance(Instance* instance)
{
    this->availableInstanceIds.push(instance->id);
    this->instances.erase(instance->id);
}

uint32_t Scene::addMaterial(ToFreeList& toFreeList, const Material* material)
{
    if (this->nextMaterialIdx >= this->mappedMaterialsArray.getSize())
    {
        this->mappedMaterialsArray.resize(toFreeList, this->mappedMaterialsArray.getSize() * 2);
    }

    const uint32_t materialIdx = this->nextMaterialIdx++;
    this->mappedMaterialsArray[materialIdx] = *material;

    return materialIdx;
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

void Scene::update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    this->isTlasDirty |= this->makeQueuedBlases(cmdList, toFreeList);

    this->mappedInstanceDescsArray.copyFromUploadBufferIfDirty(cmdList);
    this->mappedInstanceDatasArray.copyFromUploadBufferIfDirty(cmdList);

    this->mappedMaterialsArray.copyFromUploadBufferIfDirty(cmdList);

    if (!this->pendingTextures.empty())
    {
        this->uploadPendingTextures(cmdList, toFreeList);
    }

    if (this->isTlasDirty)
    {
        this->makeTlas(cmdList, toFreeList);
    }

    this->areaLightSamplingStructure.copyFromUploadBufferIfDirty(cmdList);
}

bool Scene::makeQueuedBlases(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    if (this->instancesReadyForBlasBuild.empty())
    {
        return false;
    }

    std::vector<AcsHelper::BlasBuildInputs> allBlasInputs;

    uint32_t numNewAreaLights = 0;

    for (Instance* const instance : instancesReadyForBlasBuild)
    {
        AcsHelper::BlasBuildInputs blasInputs;

        blasInputs.host_verts = &instance->host_verts;
        blasInputs.dev_verts = &managedVertsBuffer;

        if (instance->host_idxs.size() > 0)
        {
            blasInputs.host_idxs = &instance->host_idxs;
            blasInputs.dev_idxs = &managedIdxsBuffer;
        }

        blasInputs.outGeoWrapper = &instance->geoWrapper;

        allBlasInputs.push_back(blasInputs);

        numNewAreaLights += instance->host_areaLights.size();
    }

    ManagedBuffer areaLightsUploadBuffer{
        &UPLOAD_HEAP,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        false /*isResizable*/,
        true /*isMapped*/,
    };
    if (numNewAreaLights > 0)
    {
        areaLightsUploadBuffer.init(numNewAreaLights * sizeof(AreaLight));
    }

    AcsHelper::makeBlases(cmdList, toFreeList, allBlasInputs);

    BufferHelper::uavBarrier(cmdList, nullptr);

    for (const auto instance : this->instancesReadyForBlasBuild)
    {
        InstanceData& data = this->mappedInstanceDatasArray[instance->id];
        data.vertBufferOffset =
            instance->geoWrapper.vertsBufferSection.offsetBytes / static_cast<uint32_t>(sizeof(Vertex));
        data.hasIdxs = instance->geoWrapper.idxsBufferSection.sizeBytes > 0;
        data.idxBufferByteOffset = instance->geoWrapper.idxsBufferSection.offsetBytes;
        data.materialId = instance->materialId;

        instance->host_verts.clear();
        instance->host_idxs.clear();

        if (!instance->host_areaLights.empty())
        {
            ManagedBufferSection areaLightsUploadBufferSection =
                areaLightsUploadBuffer.copyFromHostVector(cmdList, toFreeList, instance->host_areaLights);
            instance->areaLightsBufferSection = this->managedAreaLightsBuffer.copyFromManagedBuffer(
                cmdList, toFreeList, areaLightsUploadBuffer, areaLightsUploadBufferSection);

            instance->host_areaLights.clear();
        }
    }

    if (numNewAreaLights > 0)
    {
        toFreeList.pushManagedBuffer(&areaLightsUploadBuffer);
    }

    this->instancesReadyForBlasBuild.clear();
    return true;
}

void Scene::makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    if (this->dev_tlas)
    {
        toFreeList.pushResource(this->dev_tlas, false);
    }

    uint32_t nextInstanceDescIdx = 0;
    uint32_t nextAreaLightSamplingIdx = 0;
    for (const auto& [instanceId, instance] : this->instances)
    {
        if (instance->isScheduledForDeletion)
        {
            continue;
        }

        D3D12_RAYTRACING_INSTANCE_DESC& instanceDesc = this->mappedInstanceDescsArray[nextInstanceDescIdx++];
        memcpy(instanceDesc.Transform, &instance->transform, sizeof(XMFLOAT3X4));
        instanceDesc.InstanceID = instanceId;
        instanceDesc.InstanceMask = 1;
        instanceDesc.AccelerationStructure = instance->geoWrapper.dev_blas->GetGPUVirtualAddress();

        if (instance->areaLightsBufferSection.sizeBytes > 0)
        {
            const uint32_t instanceNumAreaLights = instance->areaLightsBufferSection.sizeBytes / sizeof(AreaLight);
            uint32_t instanceAreaLightIdx = instance->areaLightsBufferSection.offsetBytes / sizeof(AreaLight);
            for (uint32_t idx = 0; idx < instanceNumAreaLights; ++idx)
            {
                if (nextAreaLightSamplingIdx > this->areaLightSamplingStructure.getSize())
                {
                    this->areaLightSamplingStructure.resize(toFreeList, this->areaLightSamplingStructure.getSize() * 2);
                    this->areaLightSamplingStructure[nextAreaLightSamplingIdx++] = instanceAreaLightIdx++;
                }
            }
        }
    }

    AcsHelper::TlasBuildInputs inputs;
    inputs.dev_instanceDescs = this->mappedInstanceDescsArray.getUploadBuffer(); // TODO: test if this crashes with default heap buffer
    inputs.numInstances = nextInstanceDescIdx;
    inputs.outTlas = &this->dev_tlas;

    AcsHelper::makeTlas(cmdList, toFreeList, inputs);
    this->isTlasDirty = false;

    BufferHelper::uavBarrier(cmdList, this->dev_tlas.Get());
}

void Scene::uploadPendingTextures(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
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
        const uint32_t rowPitchBytesAligned =
            (rowPitchBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        const uint32_t uploadSizeBytes = rowPitchBytesAligned * pendingTex.height;

        ComPtr<ID3D12Resource> dev_uploadBuffer =
            BufferHelper::createBasicBuffer(uploadSizeBytes, &UPLOAD_HEAP, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* host_uploadBuffer = nullptr;
        dev_uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&host_uploadBuffer));

        for (uint32_t row = 0; row < pendingTex.height; ++row)
        {
            const uint8_t* srcPtr = pendingTex.data.data() + rowPitchBytes * row;
            uint8_t* destPtr = host_uploadBuffer + rowPitchBytesAligned * row;
            memcpy(destPtr, srcPtr, rowPitchBytes);
        }

        D3D12_SUBRESOURCE_FOOTPRINT footprint = {};
        footprint.Format = texDesc.Format;
        footprint.Width = pendingTex.width;
        footprint.Height = pendingTex.height;
        footprint.Depth = 1;
        footprint.RowPitch = rowPitchBytesAligned;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = { 0, footprint };

        D3D12_TEXTURE_COPY_LOCATION srcTexLocation = {
            .pResource = dev_uploadBuffer.Get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            .PlacedFootprint = layout,
        };
        D3D12_TEXTURE_COPY_LOCATION destTexLocation = {
            .pResource = dev_texture.Get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = 0,
        };

        BufferHelper::stateTransitionResourceBarrier(
            cmdList, dev_texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyTextureRegion(&destTexLocation, 0, 0, 0, &srcTexLocation, nullptr);
        BufferHelper::stateTransitionResourceBarrier(
            cmdList, dev_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        const D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptorHandle = { heapCpuHandle.ptr + descriptorSize * pendingTex.id };
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
            .Format = texDesc.Format,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = {
                .MipLevels = 1,
            },
        };
        Renderer::device->CreateShaderResourceView(dev_texture.Get(), &srvDesc, cpuDescriptorHandle);

        this->textures[pendingTex.id] = dev_texture;
        toFreeList.pushResource(dev_uploadBuffer, true);
    }

    this->pendingTextures.clear();
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevInstanceDatasAddress() const
{
    return this->mappedInstanceDatasArray.getBufferGpuAddress();
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevMaterialsAddress() const
{
    return this->mappedMaterialsArray.getBufferGpuAddress();
}

bool Scene::hasTlas() const
{
    return this->dev_tlas != nullptr;
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevTlasAddress() const
{
    return this->dev_tlas.Get()->GetGPUVirtualAddress();
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevVertsBufferAddress() const
{
    return this->managedVertsBuffer.getBufferGpuAddress();
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevIdxsBufferAddress() const
{
    return this->managedIdxsBuffer.getBufferGpuAddress();
}

uint32_t Scene::getNumAreaLights() const
{
    return this->numAreaLights;
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevAreaLightsBufferAddress() const
{
    return this->managedAreaLightsBuffer.getBufferGpuAddress();
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevAreaLightSamplingStructureAddress() const
{
    return this->areaLightSamplingStructure.getBufferGpuAddress();
}
