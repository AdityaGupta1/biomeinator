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

#include "to_free_list.h"

#include "managed_buffer.h"
#include "rendering/renderer.h"
#include "scene/scene.h"

ID3D12Resource* ToFreeList::pushResource(const ComPtr<ID3D12Resource>& resource, bool isMapped)
{
    auto& resourceVector = (isMapped ? mappedResources : resources);
    resourceVector.push_back(resource);
    return resourceVector.back().Get();
}

void ToFreeList::pushManagedBuffer(const ManagedBuffer* buffer)
{
    this->pushResource(buffer->dev_buffer, buffer->options.isMapped);
    if (buffer->options.hasSrvDescriptor && buffer->hasValidSrvDescriptor())
    {
        this->pushDescriptor(buffer->getSrvDescriptorIdx());
    }
}

void ToFreeList::pushManagedBufferSection(const ManagedBufferSection& bufferSection)
{
    managedBufferSections.push_back(bufferSection);
}

void ToFreeList::pushInstance(Instance* instance)
{
    instances.push_back(instance);
    instance->isScheduledForDeletion = true;
    instance->scene->isTlasDirty = true;
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

    for (auto& resource : this->mappedResources)
    {
        resource->Unmap(0, nullptr);
        resource.Reset();
    }
    mappedResources.clear();

    for (const auto& bufferSection : this->managedBufferSections)
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
