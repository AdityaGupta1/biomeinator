#include "to_free_list.h"

void ToFreeList::pushResource(const ComPtr<ID3D12Resource>& resource, bool isMapped)
{
    (isMapped ? mappedResources : resources).push_back(resource);
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
}
