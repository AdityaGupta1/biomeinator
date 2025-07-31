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
    ID3D12Resource* pushResource(const ComPtr<ID3D12Resource>& resource, bool isMapped);

    // The ManagedBuffer can go out of scope but this ToFreeList will keep the underlying buffer alive until it's freed
    void pushManagedBuffer(const ManagedBuffer* buffer);
    void pushManagedBufferSection(const ManagedBufferSection& bufferSection);

    void pushInstance(Instance* instance);

    void freeAll();
};
