#pragma once

#include "rendering/dxr_includes.h"

#include <vector>
#include <utility>

class ManagedBuffer;
class ManagedBufferSection;

class ToFreeList
{
private:
    std::vector<ComPtr<ID3D12Resource>> resources;
    std::vector<ComPtr<ID3D12Resource>> mappedResources;

    std::vector<const ManagedBufferSection*> managedBufferSections;

public:
    // The caller is responsible for nulling resource if necessary.
    ID3D12Resource* pushResource(const ComPtr<ID3D12Resource>& resource, bool isMapped = false);

    void pushManagedBuffer(const ManagedBuffer* buffer);
    void pushManagedBufferSection(const ManagedBufferSection* bufferSection);

    void freeAll();
};
