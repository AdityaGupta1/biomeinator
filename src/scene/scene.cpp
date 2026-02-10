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

#include "scene.h"

#include "debug.h"
#include "rendering/buffer/acs_helper.h"
#include "rendering/buffer/buffer_helper.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/camera.h"
#include "rendering/dxr_common.h"
#include "rendering/renderer.h"
#include "util/util.h"

#include <glm/glm.hpp>

using namespace DirectX;

Instance::Instance(Scene* scene, uint32_t id)
    : scene(scene), id(id)
{}

void Instance::reset(bool alsoFreeFromScene)
{
    this->geoWrapper.blasBufferSection.free();
    this->geoWrapper.vertsBufferSection.free();
    this->geoWrapper.idxsBufferSection.free();
    this->perTriDatasBufferSection.free();

    this->areaLightsBufferSection.free();

    this->host_verts.clear();
    this->host_idxs.clear();
    this->host_perTriDatas.clear();
    this->isGeometryFinalized = false;

    if (alsoFreeFromScene)
    {
        this->scene->freeInstance(this);
    }
}

void Instance::setTransform(const DirectX::XMFLOAT3X4& transform)
{
    this->transform = transform;
}

void Instance::setTransformOffset(glm::ivec3 offset)
{
    this->transformOffset = offset;
}

void Instance::finalizeGeometry()
{
    ASSERT(this->host_verts.size() > 0);

    const uint32_t triCount = this->getTriCount();
    this->host_perTriDatas.resize(triCount);

    this->isGeometryFinalized = true;
}

void Instance::addAreaLights(const std::vector<uint32_t>& triangleIdxs)
{
    ASSERT(this->isGeometryFinalized);

    this->host_areaLights.reserve(this->host_areaLights.size() + triangleIdxs.size());

    const XMMATRIX objectToWorld = XMLoadFloat3x4(&this->transform);

    for (const uint32_t triangleIdx : triangleIdxs)
    {
        uint32_t i0 = triangleIdx * 3;
        uint32_t i1 = i0 + 1;
        uint32_t i2 = i0 + 2;
        if (!this->host_idxs.empty())
        {
            i0 = this->host_idxs[i0];
            i1 = this->host_idxs[i1];
            i2 = this->host_idxs[i2];
        }

        const uint32_t localAreaLightIdx = static_cast<uint32_t>(this->host_areaLights.size());
        this->host_areaLights.emplace_back();
        AreaLight& light = this->host_areaLights.back();

        light.instanceId = this->id;
        light.triangleIdx = triangleIdx;

        XMVECTOR p0 = XMLoadFloat3(&this->host_verts[i0].pos_OS);
        XMVECTOR p1 = XMLoadFloat3(&this->host_verts[i1].pos_OS);
        XMVECTOR p2 = XMLoadFloat3(&this->host_verts[i2].pos_OS);

        p0 = DirectX::XMVector3Transform(p0, objectToWorld);
        p1 = DirectX::XMVector3Transform(p1, objectToWorld);
        p2 = DirectX::XMVector3Transform(p2, objectToWorld);

        DirectX::XMStoreFloat3(&light.pos0_WS, p0);
        DirectX::XMStoreFloat3(&light.pos1_WS, p1);
        DirectX::XMStoreFloat3(&light.pos2_WS, p2);

        light.materialIdx = this->materialIdx;

        this->host_perTriDatas[triangleIdx].localAreaLightIdx = localAreaLightIdx;
    }
}

uint32_t Instance::getId() const
{
    return this->id;
}

uint32_t Instance::getTriCount() const
{
    return this->host_idxs.empty() ? this->host_verts.size() / 3 : this->host_idxs.size() / 3;
}

bool Instance::getIsGeometryFinalized() const
{
    return this->isGeometryFinalized;
}

void Instance::setVisible(bool visible)
{
    // TODO: may need to revisit this and check for correctness
    if (this->isVisible != visible && this->geoWrapper.blasBufferSection.isValid())
    {
        this->scene->isTlasDirty = true;
    }
    this->isVisible = visible;
}

void Instance::setMaterialIdx(uint32_t id)
{
    this->materialIdx = id;
}

void Scene::init()
{
    // these resources can be dynamically resized later
    this->managedVertsBuffer.setName(L"scene verts");
    this->managedVertsBuffer.init(1 << 16 /*bytes*/);
    this->managedIdxsBuffer.setName(L"scene idxs");
    this->managedIdxsBuffer.init(1 << 14 /*bytes*/);
    this->managedPerTriDatasBuffer.setName(L"scene perTriDatas");
    this->managedPerTriDatasBuffer.init(1 << 14 /*bytes*/);

    this->maxNumInstances = 1 << 8;
    for (uint32_t i = 0; i < Renderer::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        this->mappedInstanceDescsArrays[i].setName(L"scene instanceDescs frame " + std::to_wstring(i));
        this->mappedInstanceDescsArrays[i].init(this->maxNumInstances);
    }
    this->mappedInstanceDatasArray.setName(L"scene instanceDatas");
    this->mappedInstanceDatasArray.init(this->maxNumInstances);
    for (int instanceIdx = 0; instanceIdx < this->maxNumInstances; ++instanceIdx)
    {
        availableInstanceIds.push(instanceIdx);
    }

    this->sharedBlasUploadBuffer.setName(L"scene sharedBlasUpload");
    this->sharedBlasUploadBuffer.init(1 << 16 /*bytes*/);

    this->mappedMaterialsArray.setName(L"scene materials");
    this->mappedMaterialsArray.init(8 /*elements*/);

    this->managedAreaLightsBuffer.setName(L"scene areaLights");
    this->managedAreaLightsBuffer.init(1 << 14 /*bytes*/);
    this->areaLightSamplingStructure.setName(L"scene areaLightSamplingStructure");
    this->areaLightSamplingStructure.init(1 << 8 /*elements*/);
}

void Scene::reset()
{
    for (auto& [_, instance] : this->instances)
    {
        instance->reset(false);
    }

    this->managedVertsBuffer.reset();
    this->managedIdxsBuffer.reset();
    this->managedPerTriDatasBuffer.reset();

    this->instances.clear();
    this->instancesReadyForBlasBuild.clear();
    this->availableInstanceIds = {};
    for (uint32_t i = 0; i < Renderer::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        this->mappedInstanceDescsArrays[i].reset();
    }
    this->mappedInstanceDatasArray.reset();

    this->nextMaterialIdx = 0;

    this->isTlasDirty = false;
    this->tlasBufferSection.free();
    this->numVisibleBlasesWaitingForTlas = 0;

    for (ComPtr<ID3D12Resource>& texture : this->textures)
    {
        texture.Reset();
    }
    this->textures.clear();
    this->pendingTextures.clear();

    this->mappedMaterialsArray.reset();

    this->numAreaLights = 0;
    this->managedAreaLightsBuffer.reset();
    this->areaLightSamplingStructure.reset();
}

Instance* Scene::requestNewInstance(ToFreeList& toFreeList)
{
    if (this->availableInstanceIds.empty())
    {
        const uint32_t oldMaxNumInstances = this->maxNumInstances;

        this->maxNumInstances *= 2;
        for (uint32_t i = 0; i < Renderer::NUM_FRAMES_IN_FLIGHT; ++i)
        {
            this->mappedInstanceDescsArrays[i].resize(toFreeList, this->maxNumInstances);
        }
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

    ASSERT(newInstancePtr->host_verts.empty());
    ASSERT(newInstancePtr->host_idxs.empty());
    ASSERT(newInstancePtr->host_perTriDatas.empty());

    return newInstancePtr;
}

void Scene::markInstanceReadyForBlasBuild(Instance* instance)
{
    this->instancesReadyForBlasBuild.insert(instance);
}

void Scene::freeInstance(Instance* instance)
{
    this->availableInstanceIds.push(instance->id);
    this->instancesReadyForBlasBuild.erase(instance);
    this->instances.erase(instance->id);
    this->isTlasDirty |= instance->isVisible; // TODO: check if the instance even had a valid BLAS (be careful about order of operations in Instance::reset())
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
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
    const uint32_t texId = Renderer::sharedDescHeapAlloc.alloc(&cpuHandle);
    this->pendingTextures.push_back({ std::move(data), width, height, cpuHandle });
    return texId;
}

bool Scene::update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    this->isTlasDirty |= this->makeQueuedBlases(cmdList, toFreeList);

    bool didChange = false;

    // mappedInstanceDescsArrays don't need device buffer copy since TLAS uses upload buffer directly
    didChange |= this->mappedInstanceDatasArray.copyFromUploadBufferIfDirty(cmdList);

    didChange |= this->mappedMaterialsArray.copyFromUploadBufferIfDirty(cmdList);

    if (!this->pendingTextures.empty())
    {
        this->uploadPendingTextures(cmdList, toFreeList);
        didChange = true;
    }

    this->prevGlobalInstanceOffset = this->globalInstanceOffset;
    if (this->isTlasDirty)
    {
        const glm::ivec3 cameraPosInt_WS = Renderer::getCamera().getPosInt_WS();
        this->globalInstanceOffset = glm::ivec3(cameraPosInt_WS.x, 0, cameraPosInt_WS.z); // y = 0 to optimize for voxel mode
        this->makeTlas(cmdList, toFreeList);
        didChange = true;
    }

    didChange |= this->areaLightSamplingStructure.copyFromUploadBufferIfDirty(cmdList);

    return didChange;
}

static constexpr uint32_t maxBlasBuildsPerFrame = 8;
static constexpr uint32_t maxBlasesWaitingForTlas = 64;

bool Scene::makeQueuedBlases(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    if (this->numVisibleBlasesWaitingForTlas > maxBlasesWaitingForTlas)
    {
        return true; // force update TLAS since many BLASes are waiting
    }

    if (this->instancesReadyForBlasBuild.empty())
    {
        return numVisibleBlasesWaitingForTlas > 0;
    }

    std::vector<Instance*> instancesToBuildThisFrame;
    const uint32_t maxInstancesThisFrame =
        std::min(maxBlasBuildsPerFrame, static_cast<uint32_t>(this->instancesReadyForBlasBuild.size()));
    instancesToBuildThisFrame.reserve(maxInstancesThisFrame);
    for (Instance* const instance : this->instancesReadyForBlasBuild)
    {
        if (instance->isScheduledForDeletion)
        {
            continue;
        }

        instancesToBuildThisFrame.push_back(instance);

        if (instancesToBuildThisFrame.size() >= maxInstancesThisFrame)
        {
            break;
        }
    }

    for (Instance* const instance : instancesToBuildThisFrame)
    {
        this->instancesReadyForBlasBuild.erase(instance);
    }

    std::vector<AcsHelper::BlasBuildInputs> allBlasInputs;
    allBlasInputs.reserve(instancesToBuildThisFrame.size());

    uint32_t numPerTriDatas = 0;
    uint32_t numAreaLights = 0;

    for (Instance* const instance : instancesToBuildThisFrame)
    {
        AcsHelper::BlasBuildInputs blasInputs;

        ASSERT(instance->host_verts.size() > 0);
        blasInputs.host_verts = &instance->host_verts;
        blasInputs.dev_verts = &this->managedVertsBuffer;

        if (instance->host_idxs.size() > 0)
        {
            blasInputs.host_idxs = &instance->host_idxs;
            blasInputs.dev_idxs = &this->managedIdxsBuffer;
        }

        blasInputs.outGeoWrapper = &instance->geoWrapper;

        allBlasInputs.push_back(blasInputs);

        assert(instance->host_perTriDatas.size() > 0);
        numPerTriDatas += instance->host_perTriDatas.size();

        numAreaLights += instance->host_areaLights.size();
    }

    AcsHelper::makeBlases(cmdList, toFreeList, allBlasInputs);

    bool hadVisibleInstance = false;
    for (Instance* const instance : instancesToBuildThisFrame)
    {
        InstanceData& instanceData = this->mappedInstanceDatasArray[instance->id];
        instanceData.vertsBufferOffset =
            Util::convertByteSizeToCount<Vertex>(instance->geoWrapper.vertsBufferSection.offsetBytes);
        instanceData.hasIdxs = instance->geoWrapper.idxsBufferSection.sizeBytes > 0;
        instanceData.idxsBufferByteOffset = instance->geoWrapper.idxsBufferSection.offsetBytes;
        instanceData.materialIdx = instance->materialIdx;

        instance->host_verts.clear();
        instance->host_idxs.clear();

        const ManagedBufferSection perTriDatasUploadBufferSection =
            sharedBlasUploadBuffer.copyFromHostVector(cmdList, toFreeList, instance->host_perTriDatas);
        instance->perTriDatasBufferSection = this->managedPerTriDatasBuffer.copyFromManagedBuffer(
            cmdList, toFreeList, sharedBlasUploadBuffer, perTriDatasUploadBufferSection);
        instanceData.perTriDatasBufferOffset =
            Util::convertByteSizeToCount<PerTriangleData>(instance->perTriDatasBufferSection.offsetBytes);

        instance->host_perTriDatas.clear();
        toFreeList.pushManagedBufferSection(perTriDatasUploadBufferSection);

        if (!instance->host_areaLights.empty())
        {
            const ManagedBufferSection areaLightsUploadBufferSection =
                sharedBlasUploadBuffer.copyFromHostVector(cmdList, toFreeList, instance->host_areaLights);
            instance->areaLightsBufferSection = this->managedAreaLightsBuffer.copyFromManagedBuffer(
                cmdList, toFreeList, sharedBlasUploadBuffer, areaLightsUploadBufferSection);
            instanceData.areaLightsBufferOffset =
                Util::convertByteSizeToCount<AreaLight>(instance->areaLightsBufferSection.offsetBytes);

            instance->host_areaLights.clear();
            toFreeList.pushManagedBufferSection(areaLightsUploadBufferSection);
        }

        instanceData.transformOffset = {
            instance->transformOffset.x,
            instance->transformOffset.y,
            instance->transformOffset.z,
        };

        if (instance->isVisible)
        {
            ++numVisibleBlasesWaitingForTlas;
        }
    }

    return false;
}

void Scene::makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    if (this->hasTlas())
    {
        toFreeList.pushManagedBufferSection(tlasBufferSection);
    }

    const uint32_t frameIdx = Renderer::getFrameIndex();
    MappedArray<D3D12_RAYTRACING_INSTANCE_DESC>& currentFrameInstanceDescs = this->mappedInstanceDescsArrays[frameIdx];

    uint32_t nextInstanceDescIdx = 0;
    uint32_t nextAreaLightSamplingIdx = 0;
    for (const auto& [instanceId, instance] : this->instances)
    {
        if (!instance->isVisible || instance->isScheduledForDeletion ||
            !instance->geoWrapper.blasBufferSection.isValid())
        {
            continue;
        }

        D3D12_RAYTRACING_INSTANCE_DESC& instanceDesc = currentFrameInstanceDescs[nextInstanceDescIdx++];

        memcpy(instanceDesc.Transform, &instance->transform, sizeof(XMFLOAT3X4));
        const glm::ivec3 totalOffset = instance->transformOffset - this->globalInstanceOffset;
        for (int i = 0; i < 3; ++i)
        {
            instanceDesc.Transform[i][3] += totalOffset[i];
        }

        instanceDesc.InstanceID = instanceId;
        instanceDesc.InstanceMask = 1;
        instanceDesc.AccelerationStructure = instance->geoWrapper.blasBufferSection.getGpuVirtualAddress();

        if (instance->areaLightsBufferSection.sizeBytes > 0)
        {
            const uint32_t instanceNumAreaLights = instance->areaLightsBufferSection.sizeBytes / sizeof(AreaLight);
            uint32_t instanceAreaLightIdx = instance->areaLightsBufferSection.offsetBytes / sizeof(AreaLight);
            for (uint32_t idx = 0; idx < instanceNumAreaLights; ++idx)
            {
                if (nextAreaLightSamplingIdx >= this->areaLightSamplingStructure.getSize())
                {
                    this->areaLightSamplingStructure.resize(toFreeList, this->areaLightSamplingStructure.getSize() * 2);
                }

                this->areaLightSamplingStructure[nextAreaLightSamplingIdx++] = instanceAreaLightIdx++;
            }
        }
    }

    this->numAreaLights = nextAreaLightSamplingIdx;

    AcsHelper::TlasBuildInputs inputs;
    inputs.dev_instanceDescs = currentFrameInstanceDescs.getUploadBuffer();
    inputs.numInstances = nextInstanceDescIdx;
    inputs.updateScratchSizePtr = nullptr;
    inputs.outTlas = &this->tlasBufferSection;

    AcsHelper::makeTlas(cmdList, toFreeList, inputs);
    this->isTlasDirty = false;

    BufferHelper::uavBarrier(cmdList, this->tlasBufferSection.getBuffer()->getBuffer());

    this->numVisibleBlasesWaitingForTlas = 0;
}

const glm::ivec3& Scene::getGlobalInstanceOffset() const
{
    return this->globalInstanceOffset;
}

const glm::ivec3& Scene::getPrevGlobalInstanceOffset() const
{
    return this->prevGlobalInstanceOffset;
}

void Scene::uploadPendingTextures(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    for (const auto& pendingTex : this->pendingTextures)
    {
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = pendingTex.width;
        texDesc.Height = pendingTex.height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        texDesc.SampleDesc = SAMPLE_DESC_NO_AA;

        ComPtr<ID3D12Resource> dev_texture;
        CHECK_HRESULT(Renderer::device->CreateCommittedResource(&DEFAULT_HEAP,
                                                                D3D12_HEAP_FLAG_NONE,
                                                                &texDesc,
                                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                                nullptr,
                                                                IID_PPV_ARGS(&dev_texture)));
        dev_texture->SetName(L"scene texture");

        const uint32_t rowPitchBytes = pendingTex.width * 4;
        const uint32_t rowPitchBytesAligned =
            (rowPitchBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        const uint32_t uploadSizeBytes = rowPitchBytesAligned * pendingTex.height;

        ComPtr<ID3D12Resource> dev_uploadBuffer = BufferHelper::createBasicBuffer(uploadSizeBytes, &UPLOAD_HEAP);
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

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
            .Format = texDesc.Format,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = {
                .MipLevels = 1,
            },
        };
        Renderer::device->CreateShaderResourceView(dev_texture.Get(), &srvDesc, pendingTex.cpuHandle);

        this->textures.push_back(dev_texture);
        toFreeList.pushResource(dev_uploadBuffer, true);
    }

    this->pendingTextures.clear();
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevInstanceDatasAddress() const
{
    return this->mappedInstanceDatasArray.getGpuVirtualAddress();
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevMaterialsAddress() const
{
    return this->mappedMaterialsArray.getGpuVirtualAddress();
}

bool Scene::hasTlas() const
{
    return this->tlasBufferSection.sizeBytes > 0;
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevTlasAddress() const
{
    return this->tlasBufferSection.getGpuVirtualAddress();
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevVertsBufferAddress() const
{
    return this->managedVertsBuffer.getGpuVirtualAddress();
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevIdxsBufferAddress() const
{
    return this->managedIdxsBuffer.getGpuVirtualAddress();
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevPerTriDatasBufferAddress() const
{
    return this->managedPerTriDatasBuffer.getGpuVirtualAddress();
}

uint32_t Scene::getNumAreaLights() const
{
    return this->numAreaLights;
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevAreaLightsBufferAddress() const
{
    return this->managedAreaLightsBuffer.getGpuVirtualAddress();
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevAreaLightSamplingStructureAddress() const
{
    return this->areaLightSamplingStructure.getGpuVirtualAddress();
}
