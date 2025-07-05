#include "to_free_list.h"

#include "managed_buffer.h"
#include "rendering/scene/scene.h"

ID3D12Resource* ToFreeList::pushResource(const ComPtr<ID3D12Resource>& resource, bool isMapped)
{
    auto& resourceVector = (isMapped ? mappedResources : resources);
    resourceVector.push_back(resource);
    return resourceVector.back().Get();
}

void ToFreeList::pushManagedBuffer(const ManagedBuffer* buffer)
{
    this->pushResource(buffer->dev_buffer, buffer->isMapped);
}

void ToFreeList::pushManagedBufferSection(const ManagedBufferSection& bufferSection)
{
    managedBufferSections.push_back(bufferSection);
}

void ToFreeList::pushInstance(Instance* instance)
{
    if (instance->geoWrapper.dev_blas)
    {
        this->pushResource(instance->geoWrapper.dev_blas);
    }
    if (instance->geoWrapper.vertBufferSection.sizeBytes > 0)
    {
        this->pushManagedBufferSection(instance->geoWrapper.vertBufferSection);
    }
    if (instance->geoWrapper.idxBufferSection.sizeBytes > 0)
    {
        this->pushManagedBufferSection(instance->geoWrapper.idxBufferSection);
    }

    instances.push_back(instance);
    instance->isScheduledForDeletion = true;
    instance->scene->isTlasDirty = true;
}

void ToFreeList::freeAll()
{
    for (auto& resource : resources)
    {
        resource.Reset();
    }
    resources.clear();

    for (auto& resource : mappedResources)
    {
        resource->Unmap(0, nullptr);
        resource.Reset();
    }
    mappedResources.clear();

    for (const auto& bufferSection : managedBufferSections)
    {
        bufferSection.getManagedBuffer()->freeSection(bufferSection);
    }
    managedBufferSections.clear();

    for (Instance* instance : instances)
    {
        instance->scene->freeInstance(instance);
    }
    instances.clear();
}
