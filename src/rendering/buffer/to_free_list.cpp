#include "to_free_list.h"

#include "managed_buffer.h"

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

void ToFreeList::pushManagedBufferSection(const ManagedBufferSection* bufferSection)
{
    managedBufferSections.push_back(bufferSection);
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

    for (const auto bufferSection : managedBufferSections)
    {
        bufferSection->getManagedBuffer()->freeSection(*bufferSection);
    }
    managedBufferSections.clear();
}
