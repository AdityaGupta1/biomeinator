// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "scene.h"

#include "debug.h"
#include "rendering/buffer/acs_helper.h"
#include "rendering/buffer/buffer_helper.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/camera.h"
#include "rendering/common/common_settings.h"
#include "rendering/dxr_common.h"
#include "rendering/gpu_profiler.h"
#include "rendering/renderer.h"
#include "rendering/water_displacer.h"
#include "settings_manager.h"
#include "util/math.h"
#include "util/util.h"

#include <glm/glm.hpp>

using namespace DirectX;

Instance::Instance(Scene* scene, uint32_t id)
    : scene(scene), id(id)
{}

void Instance::stealVectors(Instance* other)
{
    ASSERT(other->host_verts.empty());
    ASSERT(other->host_tangents.empty());
    ASSERT(other->host_idxs.empty());
    ASSERT(other->host_perTriDatas.empty());
    ASSERT(other->host_ommIdxs.empty());
    ASSERT(other->host_areaLights.empty());

    this->host_verts = std::move(other->host_verts);
    this->host_tangents = std::move(other->host_tangents);
    this->host_idxs = std::move(other->host_idxs);
    this->host_perTriDatas = std::move(other->host_perTriDatas);
    this->host_ommIdxs = std::move(other->host_ommIdxs);
    this->host_areaLights = std::move(other->host_areaLights);
}

void Instance::reset(bool alsoFreeFromScene)
{
    this->geoWrapper.blasBufferSection.free();
    this->geoWrapper.vertsBufferSection.free();
    this->geoWrapper.idxsBufferSection.free();
    this->geoWrapper.ommIdxsBufferSection.free();
    this->perTriDatasBufferSection.free();
    this->tangentsBufferSection.free();
    this->areaLightsBufferSection.free();

    this->host_verts.clear();
    this->host_tangents.clear();
    this->host_idxs.clear();
    this->host_perTriDatas.clear();
    this->host_ommIdxs.clear();
    this->host_areaLights.clear();
    this->isGeometryFinalized = false;
    this->isOpaque = false;

    if (alsoFreeFromScene)
    {
        this->scene->freeInstance(this);
    }
}

void Instance::setTransform(const DirectX::XMFLOAT3X4& transform)
{
    this->transform = transform;
    if (this->tlasEntryIdx != UINT32_MAX)
    {
        memcpy(this->scene->tlasInstanceEntries[this->tlasEntryIdx].desc.Transform, &transform, sizeof(XMFLOAT3X4));
        this->scene->isTlasDirty = true;
    }
}

void Instance::setTransformOffset(glm::ivec3 offset)
{
    this->transformOffset = offset;
    if (this->tlasEntryIdx != UINT32_MAX)
    {
        this->scene->tlasInstanceEntries[this->tlasEntryIdx].transformOffset = offset;
        this->scene->isTlasDirty = true;
    }
}

void Instance::finalizeGeometry()
{
    ASSERT(this->host_verts.size() > 0);

    const uint32_t triCount = this->getTriCount();
    ASSERT(this->host_perTriDatas.size() == triCount);

    this->boundsMin_OS = glm::vec3(FLT_MAX);
    this->boundsMax_OS = glm::vec3(-FLT_MAX);
    for (const Vertex& vert : this->host_verts)
    {
        const glm::vec3 pos(vert.pos_OS.x, vert.pos_OS.y, vert.pos_OS.z);
        this->boundsMin_OS = glm::min(this->boundsMin_OS, pos);
        this->boundsMax_OS = glm::max(this->boundsMax_OS, pos);
    }

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
    return this->host_idxs.empty() ? (this->host_verts.size() / 3) : (this->host_idxs.size() / 3);
}

bool Instance::getIsGeometryFinalized() const
{
    return this->isGeometryFinalized;
}

void Instance::setVisible(bool visible)
{
    if (this->isVisible == visible)
    {
        return;
    }
    this->isVisible = visible;
    if (!this->geoWrapper.blasBufferSection.isValid())
    {
        return;
    }
    if (visible)
    {
        this->scene->pendingTlasEntryAdds.push_back(this);
    }
    else
    {
        this->scene->removeTlasEntry(this);
    }
}

void Instance::setMaterialIdx(uint32_t id)
{
    this->materialIdx = id;
}

void Instance::setIsDeformable(bool deformable)
{
    this->isDeformable = deformable;
}

void Instance::setIsOpaque(bool opaque)
{
    this->isOpaque = opaque;
}

void Scene::init()
{
    this->managedVertsBuffer.setName(L"scene verts");
    this->managedVertsBuffer.init();
    this->managedTangentsBuffer.setName(L"scene tangents");
    this->managedTangentsBuffer.init(sizeof(VertexTangent));
    this->managedIdxsBuffer.setName(L"scene idxs");
    this->managedIdxsBuffer.init();
    this->managedPerTriDatasBuffer.setName(L"scene perTriDatas");
    this->managedPerTriDatasBuffer.init();

    this->maxNumInstances = 1 << 15;
    this->instances.reserve(this->maxNumInstances);
    for (uint32_t i = 0; i < Renderer::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        this->mappedInstanceDescsArrays[i].setName(L"scene instanceDescs frame " + std::to_wstring(i));
        this->mappedInstanceDescsArrays[i].init(this->maxNumInstances, MappedArrayOptions{ .uploadOnly = true });
    }
    this->mappedInstanceDatasArray.setName(L"scene instanceDatas");
    this->mappedInstanceDatasArray.init(this->maxNumInstances);
    for (int instanceIdx = 0; instanceIdx < this->maxNumInstances; ++instanceIdx)
    {
        availableInstanceIds.push(instanceIdx);
    }

    this->sharedBlasUploadBuffer.setName(L"scene sharedBlasUpload");
    this->sharedBlasUploadBuffer.init(128ull << 20 /*bytes*/);

    this->mappedMaterialsArray.setName(L"scene materials");
    this->mappedMaterialsArray.init(8 /*elements*/);

    this->managedAreaLightsBuffer.setName(L"scene areaLights");
    this->managedAreaLightsBuffer.init();
    this->areaLightSamplingStructure.setName(L"scene areaLightSamplingStructure");
    // makeTlas rewrites every live entry whenever it marks this dirty, so per-frame upload
    // staging is safe here. It is required: the light tree's emitter_collect indexes its
    // UAVs with values read straight out of this buffer, so a torn upload becomes a wild
    // GPU write rather than a wrong-looking frame.
    this->areaLightSamplingStructure.init(1 << 21 /*elements*/, { .perFrameUpload = true });
}

void Scene::reset()
{
    this->invalidateRadianceHistory();
    for (auto& [_, instance] : this->instances)
    {
        instance->reset(false);
    }

    this->managedVertsBuffer.reset();
    this->managedTangentsBuffer.reset();
    this->managedIdxsBuffer.reset();
    this->managedPerTriDatasBuffer.reset();

    this->instances.clear();
    this->instancesReadyForBlasBuild.clear();
    this->deformableInstances.clear();
    this->animatedDeformablesDirty = true;
    this->tlasInstanceEntries.clear();
    this->tlasEntriesNeedCompaction = false;
    this->pendingTlasEntryAdds.clear();
    this->areaLightDenseIdxs.clear();
    this->availableInstanceIds = {};
    for (uint32_t i = 0; i < Renderer::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        this->mappedInstanceDescsArrays[i].reset();
    }
    this->mappedInstanceDatasArray.reset();

    this->sharedBlasUploadBuffer.reset();

    this->isTlasDirty = false;
    this->tlasBufferSection.free();

    this->nextMaterialIdx = 0;

    for (ComPtr<ID3D12Resource>& texture : this->textures)
    {
        texture.Reset();
    }
    this->textures.clear();
    this->pendingTextures.clear();

    this->mappedMaterialsArray.reset();

    this->numAreaLights = 0;
    this->areaLightSparseCount = 0;
    this->areaLightTopologyChanged = false;
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

    if (!instancesToReuse.empty())
    {
        newInstancePtr->stealVectors(instancesToReuse.front().get());
        instancesToReuse.pop();
    }

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
    if (this->deformableInstances.erase(instance) > 0)
    {
        // Also drop it from the cached subset, which is compared against on the next rebuild
        std::erase(this->animatedDeformables, instance);
        this->animatedDeformablesDirty = true;
    }

    auto instanceIter = this->instances.find(instance->id);
    ASSERT(instanceIter != this->instances.end());
    this->instancesToReuse.push(std::move(instanceIter->second));
    this->instances.erase(instanceIter);

    this->removeTlasEntry(instance);
}

uint32_t Scene::addMaterial(ToFreeList& toFreeList, const Material* material)
{
    ASSERT((material->flags & MATERIAL_FLAGS_DIFFUSE_OR_GLOSSY_TRANSMISSION) != MATERIAL_FLAGS_DIFFUSE_OR_GLOSSY_TRANSMISSION,
           "Diffuse and glossy transmission are mutually exclusive");
    const bool isTransmissionOnly = (material->flags & MATERIAL_FLAGS_GLOSSY) == MATERIAL_FLAG_GLOSSY_TRANSMISSION;
    ASSERT(!(isTransmissionOnly && material->roughness > 0.f),
           "Transmission-only materials must be perfectly specular");

    if (this->nextMaterialIdx >= this->mappedMaterialsArray.getSize())
    {
        this->mappedMaterialsArray.resize(toFreeList, this->mappedMaterialsArray.getSize() * 2);
    }

    const uint32_t materialIdx = this->nextMaterialIdx++;
    this->mappedMaterialsArray[materialIdx] = *material;
    this->mappedMaterialsArray.markDirty(materialIdx);

    return materialIdx;
}

uint32_t Scene::addTexture(std::vector<std::vector<uint8_t>>&& mipData, uint32_t width, uint32_t height, DXGI_FORMAT format)
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
    const uint32_t texId = Renderer::sharedDescHeapAlloc.alloc(&cpuHandle);
    std::vector<std::vector<std::vector<uint8_t>>> sliceMipData;
    sliceMipData.emplace_back(std::move(mipData));
    this->pendingTextures.push_back({ std::move(sliceMipData), width, height, 1u, cpuHandle, format });
    return texId;
}

uint32_t Scene::addTexture(std::vector<uint8_t>&& mip0, uint32_t width, uint32_t height, DXGI_FORMAT format)
{
    std::vector<std::vector<uint8_t>> mipData;
    mipData.emplace_back(std::move(mip0));
    return this->addTexture(std::move(mipData), width, height, format);
}

uint32_t Scene::addTextureArray(std::vector<std::vector<std::vector<uint8_t>>>&& sliceMipData,
                                uint32_t width,
                                uint32_t height,
                                DXGI_FORMAT format)
{
    // 1 slice must go through addTexture() so SRV dim matches material's array flag.
    ASSERT(sliceMipData.size() > 1);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
    const uint32_t texId = Renderer::sharedDescHeapAlloc.alloc(&cpuHandle);
    const uint32_t arraySize = static_cast<uint32_t>(sliceMipData.size());
    this->pendingTextures.push_back({ std::move(sliceMipData), width, height, arraySize, cpuHandle, format });
    return texId;
}

void Scene::invalidateRadianceHistory()
{
    this->radianceHistoryInvalidated = true;
}

bool Scene::consumeRadianceHistoryInvalidation()
{
    const bool invalidated = this->radianceHistoryInvalidated;
    this->radianceHistoryInvalidated = false;
    return invalidated;
}

bool Scene::update(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, float waveTime)
{
    this->areaLightTopologyChanged = false;

    {
        GPU_PROFILE_SCOPE(cmdList, "blas build");
        this->makeQueuedBlases(cmdList, toFreeList);
    }

    this->updateDeformableInstances(cmdList, toFreeList, waveTime);

    bool didChange = false;

    // mappedInstanceDescsArrays don't need device buffer copy since TLAS uses upload buffer directly
    didChange |= this->mappedInstanceDatasArray.copyFromUploadBufferIfDirty(cmdList);

    const bool materialsChanged = this->mappedMaterialsArray.copyFromUploadBufferIfDirty(cmdList);
    didChange |= materialsChanged;
    if (materialsChanged || !this->pendingTextures.empty())
        this->invalidateRadianceHistory();

    if (!this->pendingTextures.empty())
    {
        this->uploadPendingTextures(cmdList, toFreeList);
        didChange = true;
    }

    for (Instance* const instance : this->pendingTlasEntryAdds)
    {
        if (instance->isVisible && !instance->isScheduledForDeletion && instance->geoWrapper.blasBufferSection.isValid())
        {
            this->addTlasEntry(instance, toFreeList);
        }
    }
    this->pendingTlasEntryAdds.clear();

    this->prevGlobalInstanceOffset = this->globalInstanceOffset;
    // Intentionally rebuild the TLAS every frame once one exists, even on frames with no
    // deformable instances: BLAS refits change the AABBs the TLAS caches, and the steady
    // rebuild also smooths the frame-pacing spikes caused by bursty rebuilds. Deformation
    // alone doesn't reset accumulation, so didChange only reflects actual topology changes
    // (isTlasDirty).
    if (this->isTlasDirty || this->hasTlas())
    {
        didChange |= this->isTlasDirty;
        const glm::ivec3 cameraPosInt_WS = Renderer::getCamera().getPosInt_WS();
        this->globalInstanceOffset = glm::ivec3(cameraPosInt_WS.x, 0, cameraPosInt_WS.z); // y = 0 to optimize for voxel mode
        // The entries and the area light sampling structure were kept up to date as instances
        // came and went, so this only re-applies the global offset
        GPU_PROFILE_SCOPE(cmdList, "tlas build");
        this->makeTlas(cmdList, toFreeList);
    }

    didChange |= this->areaLightSamplingStructure.copyFromUploadBufferIfDirty(cmdList);

    return didChange;
}

void Scene::updateDeformableInstances(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, float waveTime)
{
    if (this->deformableInstances.empty())
    {
        return;
    }
    std::vector<WaterDisplacer::DispatchInputs> allDispatchInputs;
    std::vector<AcsHelper::GeometryWrapper*> geoWrappers;
    const auto addDispatch = [&](Instance* const instance, const float waveScale)
    {
        WaterDisplacer::DispatchInputs dispatchInputs;
        dispatchInputs.vertsBufferOffset =
            Util::convertByteSizeToCount<Vertex>(instance->geoWrapper.vertsBufferSection.offsetBytes);
        dispatchInputs.vertCount = Util::convertByteSizeToCount<Vertex>(instance->geoWrapper.vertsBufferSection.sizeBytes);
        dispatchInputs.transformOffset = instance->transformOffset;
        dispatchInputs.waveScale = waveScale;
        allDispatchInputs.push_back(dispatchInputs);

        geoWrappers.push_back(&instance->geoWrapper);
    };

    if (this->animatedDeformablesDirty)
    {
        std::vector<Instance*> previouslyAnimated = std::move(this->animatedDeformables);
        this->animatedDeformables.clear();
        for (Instance* const instance : this->deformableInstances)
        {
            if (this->isDeformableAnimated(instance))
            {
                this->animatedDeformables.push_back(instance);
            }
        }
        this->animatedDeformablesDirty = false;

        // A chunk normally leaves the set already at rest height because the fades end inside
        // the set's limits, but a camera jump or a fast turn can take one out mid-wave; one
        // flattening pass makes what it keeps for good match its static neighbours
        std::sort(this->animatedDeformables.begin(), this->animatedDeformables.end());
        for (Instance* const instance : previouslyAnimated)
        {
            if (!std::binary_search(this->animatedDeformables.begin(), this->animatedDeformables.end(), instance))
            {
                addDispatch(instance, 0.f /*waveScale*/);
            }
        }
    }

    for (Instance* const instance : this->animatedDeformables)
    {
        addDispatch(instance, 1.f /*waveScale*/);
    }

    if (geoWrappers.empty())
    {
        return;
    }
    GPU_PROFILE_SCOPE(cmdList, "deformables");

    // whole-resource transitions also cover terrain verts, so the displacement pass must not
    // overlap other passes reading verts (BLAS build/refit reads them in NON_PIXEL_SHADER_RESOURCE)
    ID3D12Resource* dev_vertsResource = this->managedVertsBuffer.getBuffer();
    BufferHelper::stateTransitionResourceBarrier(cmdList,
                                                 dev_vertsResource,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    {
        GPU_PROFILE_SCOPE(cmdList, "water displace");
        WaterDisplacer::dispatch(
            cmdList, this->managedVertsBuffer.getGpuVirtualAddress(), waveTime, this->waveFade, allDispatchInputs);
    }

    BufferHelper::uavBarrier(cmdList, dev_vertsResource);
    BufferHelper::stateTransitionResourceBarrier(cmdList,
                                                 dev_vertsResource,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    {
        GPU_PROFILE_SCOPE(cmdList, "blas refit");
        AcsHelper::updateBlases(cmdList, toFreeList, geoWrappers);
    }
}

void Scene::makeQueuedBlases(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    if (this->instancesReadyForBlasBuild.empty())
    {
        return;
    }

    std::vector<Instance*> instancesToBuildThisFrame;
    const uint32_t maxInstancesThisFrame = std::min(SettingsManager::getAsUint("maxBlasBuildsPerFrame"),
                                                    static_cast<uint32_t>(this->instancesReadyForBlasBuild.size()));
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

    if (instancesToBuildThisFrame.empty())
    {
        return;
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

        if (instance->host_idxs.size() > 0)
        {
            blasInputs.host_idxs = &instance->host_idxs;
        }

        if (instance->host_ommIdxs.size() > 0)
        {
            ASSERT(instance->host_ommIdxs.size() == instance->getTriCount());
            blasInputs.host_ommIdxs = &instance->host_ommIdxs;
        }

        blasInputs.allowUpdate = instance->isDeformable;
        blasInputs.isOpaque = instance->isOpaque;
        blasInputs.outGeoWrapper = &instance->geoWrapper;

        allBlasInputs.push_back(blasInputs);

        assert(instance->host_perTriDatas.size() > 0);
        numPerTriDatas += instance->host_perTriDatas.size();

        numAreaLights += instance->host_areaLights.size();
    }

    AcsHelper::makeBlases(cmdList, toFreeList, &this->managedVertsBuffer, &this->managedIdxsBuffer, allBlasInputs);
    this->numBlasBuilds += static_cast<uint32_t>(allBlasInputs.size());

    this->managedPerTriDatasBuffer.beginBatchCopy(cmdList);
    this->managedAreaLightsBuffer.beginBatchCopy(cmdList);

    for (Instance* const instance : instancesToBuildThisFrame)
    {
        InstanceData instanceData{};
        instanceData.vertsBufferOffset =
            Util::convertByteSizeToCount<Vertex>(instance->geoWrapper.vertsBufferSection.offsetBytes);
        instanceData.hasIdxs = instance->geoWrapper.idxsBufferSection.sizeBytes > 0;
        instanceData.idxsBufferByteOffset = instance->geoWrapper.idxsBufferSection.offsetBytes;
        instanceData.materialIdx = instance->materialIdx;
        instanceData.tangentsBufferOffset = TANGENT_BUFFER_OFFSET_INVALID;
        if (!instance->host_tangents.empty())
        {
            ASSERT(instance->host_tangents.size() == instance->host_verts.size());
            const ManagedBufferSection upload =
                sharedBlasUploadBuffer.copyFromHostVector(cmdList, toFreeList, instance->host_tangents);
            // This committed buffer can resize, so keep its copy transitions unbatched.
            instance->tangentsBufferSection = this->managedTangentsBuffer.copyFromManagedBuffer(
                cmdList, toFreeList, sharedBlasUploadBuffer, upload);
            instanceData.tangentsBufferOffset =
                Util::convertByteSizeToCount<VertexTangent>(instance->tangentsBufferSection.offsetBytes);
            toFreeList.pushManagedBufferSection(upload);
        }

        const ManagedBufferSection perTriDatasUploadBufferSection =
            sharedBlasUploadBuffer.copyFromHostVector(cmdList, toFreeList, instance->host_perTriDatas);
        instance->perTriDatasBufferSection = this->managedPerTriDatasBuffer.copyFromManagedBuffer(
            cmdList, toFreeList, sharedBlasUploadBuffer, perTriDatasUploadBufferSection);
        instanceData.perTriDatasBufferOffset =
            Util::convertByteSizeToCount<PerTriangleData>(instance->perTriDatasBufferSection.offsetBytes);

        toFreeList.pushManagedBufferSection(perTriDatasUploadBufferSection);

        if (!instance->host_areaLights.empty())
        {
            const ManagedBufferSection areaLightsUploadBufferSection =
                sharedBlasUploadBuffer.copyFromHostVector(cmdList, toFreeList, instance->host_areaLights);
            instance->areaLightsBufferSection = this->managedAreaLightsBuffer.copyFromManagedBuffer(
                cmdList, toFreeList, sharedBlasUploadBuffer, areaLightsUploadBufferSection);
            instanceData.areaLightsBufferOffset =
                Util::convertByteSizeToCount<AreaLight>(instance->areaLightsBufferSection.offsetBytes);

            toFreeList.pushManagedBufferSection(areaLightsUploadBufferSection);
        }

        instanceData.transformOffset = {
            instance->transformOffset.x,
            instance->transformOffset.y,
            instance->transformOffset.z,
        };

        this->mappedInstanceDatasArray[instance->id] = instanceData;
        this->mappedInstanceDatasArray.markDirty(instance->id);

        if (instance->isDeformable)
        {
            this->deformableInstances.insert(instance);
            this->animatedDeformablesDirty = true;
        }

        if (instance->isVisible)
        {
            this->addTlasEntry(instance, toFreeList);
        }
    }

    this->managedPerTriDatasBuffer.endBatchCopy(cmdList);
    this->managedAreaLightsBuffer.endBatchCopy(cmdList);
}

void Scene::stageAreaLightSamplingRange(ToFreeList& toFreeList, const uint32_t start, const uint32_t count)
{
    if (count == 0)
    {
        return;
    }
    if (this->areaLightSamplingStructure.getSize() < start + count)
    {
        this->areaLightSamplingStructure.resize(toFreeList, Util::nextPow2AtLeast(1u, start + count));
    }
    memcpy(&this->areaLightSamplingStructure[start], &this->areaLightDenseIdxs[start], count * sizeof(uint32_t));
    this->areaLightSamplingStructure.markDirtyRange(start, start + count);
    this->areaLightTopologyChanged = true;
}

void Scene::addTlasEntry(Instance* const instance, ToFreeList& toFreeList)
{
    ASSERT(instance->tlasEntryIdx == UINT32_MAX);
    instance->tlasEntryIdx = static_cast<uint32_t>(this->tlasInstanceEntries.size());

    TlasInstanceEntry entry = {};
    memcpy(entry.desc.Transform, &instance->transform, sizeof(XMFLOAT3X4));
    entry.desc.InstanceID = instance->id;
    entry.desc.InstanceMask = 1;
    entry.desc.AccelerationStructure = instance->geoWrapper.blasBufferSection.getGpuVirtualAddress();
    entry.transformOffset = instance->transformOffset;
    entry.instance = instance;
    entry.areaLightSparseOffset = instance->areaLightsBufferSection.offsetBytes / sizeof(AreaLight);
    entry.numAreaLights = instance->areaLightsBufferSection.sizeBytes / sizeof(AreaLight);
    this->tlasInstanceEntries.push_back(entry);

    const uint32_t denseStart = static_cast<uint32_t>(this->areaLightDenseIdxs.size());
    for (uint32_t idx = 0; idx < entry.numAreaLights; ++idx)
    {
        this->areaLightDenseIdxs.push_back(entry.areaLightSparseOffset + idx);
    }
    this->numAreaLights = static_cast<uint32_t>(this->areaLightDenseIdxs.size());
    this->areaLightSparseCount = std::max(this->areaLightSparseCount, entry.areaLightSparseOffset + entry.numAreaLights);
    this->stageAreaLightSamplingRange(toFreeList, denseStart, entry.numAreaLights);

    this->isTlasDirty = true;
}

void Scene::removeTlasEntry(Instance* const instance)
{
    if (instance->tlasEntryIdx == UINT32_MAX)
    {
        return;
    }
    this->tlasInstanceEntries[instance->tlasEntryIdx].instance = nullptr;
    instance->tlasEntryIdx = UINT32_MAX;
    this->tlasEntriesNeedCompaction = true;
    this->isTlasDirty = true;
}

// Removals only mark entries dead, so a frame with any number of them pays one pass here
void Scene::compactTlasEntries()
{
    uint32_t numKept = 0;
    this->areaLightDenseIdxs.clear();
    this->areaLightSparseCount = 0;
    for (const TlasInstanceEntry& entry : this->tlasInstanceEntries)
    {
        if (entry.instance == nullptr)
        {
            continue;
        }
        entry.instance->tlasEntryIdx = numKept;
        this->tlasInstanceEntries[numKept++] = entry;
        for (uint32_t idx = 0; idx < entry.numAreaLights; ++idx)
        {
            this->areaLightDenseIdxs.push_back(entry.areaLightSparseOffset + idx);
        }
        this->areaLightSparseCount = std::max(this->areaLightSparseCount, entry.areaLightSparseOffset + entry.numAreaLights);
    }
    this->tlasInstanceEntries.resize(numKept);
    this->numAreaLights = static_cast<uint32_t>(this->areaLightDenseIdxs.size());
    this->tlasEntriesNeedCompaction = false;
}

void Scene::makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    if (this->hasTlas())
    {
        toFreeList.pushManagedBufferSection(tlasBufferSection);
    }

    const uint32_t frameIdx = Renderer::getFrameIndex();
    MappedArray<D3D12_RAYTRACING_INSTANCE_DESC>& currentFrameInstanceDescs = this->mappedInstanceDescsArrays[frameIdx];

    if (this->tlasEntriesNeedCompaction)
    {
        this->compactTlasEntries();
        this->stageAreaLightSamplingRange(toFreeList, 0, this->numAreaLights);
    }

    const uint32_t numInstances = static_cast<uint32_t>(this->tlasInstanceEntries.size());
    for (uint32_t i = 0; i < numInstances; ++i)
    {
        const TlasInstanceEntry& entry = this->tlasInstanceEntries[i];
        D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = entry.desc;
        const glm::ivec3 totalOffset = entry.transformOffset - this->globalInstanceOffset;
        for (int k = 0; k < 3; ++k)
        {
            instanceDesc.Transform[k][3] += totalOffset[k];
        }
        currentFrameInstanceDescs[i] = instanceDesc;
    }

    AcsHelper::TlasBuildInputs inputs;
    inputs.dev_instanceDescs = currentFrameInstanceDescs.getUploadBuffer();
    inputs.numInstances = numInstances;
    inputs.updateScratchSizePtr = nullptr;
    inputs.outTlas = &this->tlasBufferSection;

    AcsHelper::makeTlas(cmdList, toFreeList, inputs);
    this->isTlasDirty = false;

    BufferHelper::uavBarrier(cmdList, this->tlasBufferSection.getBuffer()->getBuffer());
}

void Scene::setDeformableAnimation(const glm::vec2 centerXZ_WS, const float animRadius, const float fadeStart, const float fadeEnd)
{
    if (centerXZ_WS != this->deformableAnimCenterXZ_WS || animRadius != this->deformableAnimRadius)
    {
        this->animatedDeformablesDirty = true;
    }
    this->deformableAnimCenterXZ_WS = centerXZ_WS;
    this->deformableAnimRadius = animRadius;
    this->waveFade.fadeStart = fadeStart;
    this->waveFade.fadeEnd = fadeEnd;
}

void Scene::setWaveFrustum(const glm::vec3 cameraPos_WS, const std::array<glm::vec3, 4>& sideNormals_WS)
{
    this->waveFade.cameraPos_WS = { cameraPos_WS.x, cameraPos_WS.y, cameraPos_WS.z };
    for (uint32_t i = 0; i < 4; ++i)
    {
        const glm::vec3& normal = sideNormals_WS[i];
        DirectX::XMFLOAT3& dest = this->waveFade.frustumNormals_WS[i].normal_WS;
        if (dest.x != normal.x || dest.y != normal.y || dest.z != normal.z)
        {
            this->animatedDeformablesDirty = true;
        }
        dest = { normal.x, normal.y, normal.z };
    }
    this->waveFrustumSet = true;
}

// Conservative: the padding is wider than the shader's outer band, and the chunk's bounding
// sphere is added on top, so anything the shaders could still animate is in the set
bool Scene::isDeformableAnimated(const Instance* const instance) const
{
    const glm::vec2 offsetXZ = { instance->transformOffset.x, instance->transformOffset.z };
    if (glm::distance(offsetXZ, this->deformableAnimCenterXZ_WS) > this->deformableAnimRadius)
    {
        return false;
    }
    if (!this->waveFrustumSet)
    {
        return true;
    }

    const glm::vec3 cameraPos_WS(this->waveFade.cameraPos_WS.x, this->waveFade.cameraPos_WS.y, this->waveFade.cameraPos_WS.z);
    const glm::vec3 center_WS = glm::vec3(instance->transformOffset) + 0.5f * (instance->boundsMin_OS + instance->boundsMax_OS);
    const float radius = 0.5f * glm::length(instance->boundsMax_OS - instance->boundsMin_OS);
    const glm::vec3 toCenter_WS = center_WS - cameraPos_WS;
    const float dist = glm::length(toCenter_WS);
    if (dist - radius < WATER_FOV_EXEMPT_FAR)
    {
        return true;
    }

    // Padding past the shader's outer band covers the camera moving within its chunk between
    // rebuilds (the radial center is chunk-quantized, this test is not)
    constexpr float membershipPadSin = WATER_FOV_PAD_OUTER_SIN + 0.25f;
    for (const WaveFadeFrustumNormal& plane : this->waveFade.frustumNormals_WS)
    {
        const glm::vec3 normal(plane.normal_WS.x, plane.normal_WS.y, plane.normal_WS.z);
        const float signedDist = glm::dot(normal, toCenter_WS);
        if (signedDist < -(membershipPadSin * dist + radius))
        {
            return false;
        }
    }
    return true;
}

const glm::ivec3& Scene::getGlobalInstanceOffset() const
{
    return this->globalInstanceOffset;
}

const glm::ivec3& Scene::getPrevGlobalInstanceOffset() const
{
    return this->prevGlobalInstanceOffset;
}

struct MipLayout
{
    uint32_t offset;
    uint32_t rowPitchBytes;
    uint32_t rowPitchBytesAligned;
    uint32_t width;
    uint32_t height;
};

void Scene::uploadPendingTextures(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList)
{
    for (const auto& pendingTex : this->pendingTextures)
    {
        ASSERT(pendingTex.width > 0 && pendingTex.height > 0);
        ASSERT(pendingTex.arraySize > 0);
        ASSERT(pendingTex.sliceMipData.size() == pendingTex.arraySize);
        const uint32_t numMips = static_cast<uint32_t>(pendingTex.sliceMipData[0].size());
        ASSERT(numMips > 0);

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = pendingTex.width;
        texDesc.Height = pendingTex.height;
        texDesc.DepthOrArraySize = static_cast<UINT16>(pendingTex.arraySize);
        texDesc.MipLevels = numMips;
        texDesc.Format = pendingTex.format;
        texDesc.SampleDesc = SAMPLE_DESC_NO_AA;

        ComPtr<ID3D12Resource> dev_texture;
        CHECK_HRESULT(Renderer::getDevice()->CreateCommittedResource(&DEFAULT_HEAP,
                                                                     D3D12_HEAP_FLAG_NONE,
                                                                     &texDesc,
                                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                                     nullptr,
                                                                     IID_PPV_ARGS(&dev_texture)));
        dev_texture->SetName(L"scene texture");

        // One entry per (slice, mip); each mip start aligned to D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT.
        std::vector<MipLayout> mipLayouts(static_cast<size_t>(pendingTex.arraySize) * numMips);
        uint32_t totalUploadSizeBytes = 0;
        for (uint32_t slice = 0; slice < pendingTex.arraySize; ++slice)
        {
            ASSERT(pendingTex.sliceMipData[slice].size() == numMips);
            for (uint32_t m = 0; m < numMips; ++m)
            {
                const uint32_t mipWidth = std::max(1u, pendingTex.width >> m);
                const uint32_t mipHeight = std::max(1u, pendingTex.height >> m);
                const uint32_t rowPitch = mipWidth * 4;
                const size_t expectedMipSizeBytes = static_cast<size_t>(rowPitch) * mipHeight;
                ASSERT(pendingTex.sliceMipData[slice][m].size() == expectedMipSizeBytes);
                const uint32_t rowPitchAligned = MathUtil::roundUpToPow2(rowPitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
                totalUploadSizeBytes = MathUtil::roundUpToPow2(totalUploadSizeBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
                mipLayouts[slice * numMips + m] = { totalUploadSizeBytes, rowPitch, rowPitchAligned, mipWidth, mipHeight };
                totalUploadSizeBytes += rowPitchAligned * mipHeight;
            }
        }

        ComPtr<ID3D12Resource> dev_uploadBuffer = BufferHelper::createBasicBuffer(totalUploadSizeBytes, &UPLOAD_HEAP);
        uint8_t* host_uploadBuffer = nullptr;
        dev_uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&host_uploadBuffer));

        for (uint32_t slice = 0; slice < pendingTex.arraySize; ++slice)
        {
            for (uint32_t m = 0; m < numMips; ++m)
            {
                const MipLayout& layout = mipLayouts[slice * numMips + m];
                for (uint32_t row = 0; row < layout.height; ++row)
                {
                    const uint8_t* srcPtr = pendingTex.sliceMipData[slice][m].data() + layout.rowPitchBytes * row;
                    uint8_t* destPtr = host_uploadBuffer + layout.offset + layout.rowPitchBytesAligned * row;
                    memcpy(destPtr, srcPtr, layout.rowPitchBytes);
                }
            }
        }

        dev_uploadBuffer->Unmap(0, nullptr);

        BufferHelper::stateTransitionResourceBarrier(
            cmdList, dev_texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

        for (uint32_t slice = 0; slice < pendingTex.arraySize; ++slice)
        {
            for (uint32_t m = 0; m < numMips; ++m)
            {
                const MipLayout& layout = mipLayouts[slice * numMips + m];

                D3D12_SUBRESOURCE_FOOTPRINT footprint = {};
                footprint.Format = texDesc.Format;
                footprint.Width = layout.width;
                footprint.Height = layout.height;
                footprint.Depth = 1;
                footprint.RowPitch = layout.rowPitchBytesAligned;

                D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed = { layout.offset, footprint };

                D3D12_TEXTURE_COPY_LOCATION srcTexLocation = {
                    .pResource = dev_uploadBuffer.Get(),
                    .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                    .PlacedFootprint = placed,
                };
                D3D12_TEXTURE_COPY_LOCATION destTexLocation = {
                    .pResource = dev_texture.Get(),
                    .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                    .SubresourceIndex = D3D12CalcSubresource(m, slice, 0, numMips, pendingTex.arraySize),
                };

                cmdList->CopyTextureRegion(&destTexLocation, 0, 0, 0, &srcTexLocation, nullptr);
            }
        }

        BufferHelper::stateTransitionResourceBarrier(
            cmdList, dev_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
            .Format = texDesc.Format,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        };
        if (pendingTex.arraySize > 1)
        {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray = {
                .MostDetailedMip = 0,
                .MipLevels = numMips,
                .FirstArraySlice = 0,
                .ArraySize = pendingTex.arraySize,
            };
        }
        else
        {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D = {
                .MipLevels = numMips,
            };
        }
        Renderer::getDevice()->CreateShaderResourceView(dev_texture.Get(), &srvDesc, pendingTex.cpuHandle);

        this->textures.push_back(dev_texture);
        toFreeList.pushResource(dev_uploadBuffer);
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

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevTangentsBufferAddress() const
{
    return this->managedTangentsBuffer.getGpuVirtualAddress();
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

uint32_t Scene::getAreaLightSparseCount() const
{
    return this->areaLightSparseCount;
}

bool Scene::didAreaLightTopologyChange() const
{
    return this->areaLightTopologyChanged;
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevAreaLightsBufferAddress() const
{
    return this->managedAreaLightsBuffer.getGpuVirtualAddress();
}

D3D12_GPU_VIRTUAL_ADDRESS Scene::getDevAreaLightSamplingStructureAddress() const
{
    return this->areaLightSamplingStructure.getGpuVirtualAddress();
}
