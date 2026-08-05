// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "to_free_list.h"

#include "managed_buffer.h"
#include "rendering/renderer.h"
#include "scene/scene.h"

ID3D12Resource* ToFreeList::pushResource(const ComPtr<ID3D12Resource>& resource)
{
    resources.push_back(resource);
    return resources.back().Get();
}

void ToFreeList::pushManagedBufferSection(const ManagedBufferSection& bufferSection)
{
    managedBufferSections.push_back(bufferSection);
}

void ToFreeList::pushInstance(Instance* instance)
{
    instances.push_back(instance);
    instance->isScheduledForDeletion = true;
    instance->setVisible(false);
}

void ToFreeList::pushDescriptor(const uint32_t idx)
{
    this->descriptorIdxs.push_back(idx);
}

void ToFreeList::freeAll()
{
    for (auto& descriptorIdx : this->descriptorIdxs)
    {
        Renderer::sharedDescHeapAlloc.free(descriptorIdx);
    }

    for (auto& resource : this->resources)
    {
        resource.Reset();
    }
    resources.clear();

    for (auto& bufferSection : this->managedBufferSections)
    {
        bufferSection.free();
    }
    managedBufferSections.clear();

    for (Instance* instance : this->instances)
    {
        instance->reset(true /*alsoFreeFromScene*/);
    }
    instances.clear();
}
