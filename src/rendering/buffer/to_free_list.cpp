#include "to_free_list.h"

#include "managed_buffer.h"

void ToFreeList::pushResource(const ComPtr<ID3D12Resource>& resource, bool isMapped)
{
    (isMapped ? mappedResources : resources).push_back(resource);
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
