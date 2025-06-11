#pragma once

#include "dxr_includes.h"

#include <vector>
#include <utility>

class ManagedBuffer;
class ManagedBufferSection;

class ToFreeList
{
private:
    std::vector<ComPtr<ID3D12Resource>> resources;
    std::vector<ComPtr<ID3D12Resource>> mappedResources;

    std::vector<std::pair<ManagedBuffer*, const ManagedBufferSection*>> managedBufferSections;

public:
    // The caller is responsible for nulling the pointer if necessary.
    void pushResource(const ComPtr<ID3D12Resource>& resource, bool isMapped = false);

    void pushManagedBuffer(const ManagedBuffer* buffer);
    void pushManagedBufferSection(ManagedBuffer* buffer, const ManagedBufferSection* bufferSection);

    void freeAll();
};
