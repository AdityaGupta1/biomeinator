// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "rendering/dxr_includes.h"

#include <deque>
#include <vector>

class ManagedBuffer;
struct ManagedBufferSection;
class Instance;
class Scene;

class ToFreeList
{
private:
    std::vector<ComPtr<ID3D12Resource>> resources;
    std::vector<ComPtr<ID3D12Resource>> mappedResources;

    std::vector<ManagedBufferSection> managedBufferSections;

    std::deque<Instance*> instances;

    std::vector<uint32_t> descriptorIdxs;

public:
    // The caller is responsible for nulling the ComPtr if necessary.
    ID3D12Resource* pushResource(const ComPtr<ID3D12Resource>& resource, bool isMapped);

    void pushManagedBufferSection(const ManagedBufferSection& bufferSection);

    void pushInstance(Instance* instance);

    void pushDescriptor(const uint32_t idx);

    void freeAll();
};
