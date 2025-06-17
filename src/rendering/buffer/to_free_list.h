#pragma once

#include "rendering/dxr_includes.h"

#include <vector>

class ManagedBuffer;
class ManagedBufferSection;
class Instance;
class Scene;

class ToFreeList
{
private:
    std::vector<ComPtr<ID3D12Resource>> resources;
    std::vector<ComPtr<ID3D12Resource>> mappedResources;

    std::vector<ManagedBufferSection> managedBufferSections;
    std::vector<Instance*> instances;

public:
    // The caller is responsible for nulling the ComPtr if necessary.
    ID3D12Resource* pushResource(const ComPtr<ID3D12Resource>& resource, bool isMapped = false);

    void pushManagedBuffer(const ManagedBuffer* buffer);
    void pushManagedBufferSection(const ManagedBufferSection& bufferSection);

    void pushInstance(Instance* instance);

    void freeAll();
};
