#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <vector>

using Microsoft::WRL::ComPtr;

class ToFreeList
{
private:
    std::vector<ComPtr<ID3D12Resource>> resources;
    std::vector<ComPtr<ID3D12Resource>> mappedResources;

public:
    // The caller is responsible for nulling the pointer if necessary.
    void pushResource(const ComPtr<ID3D12Resource>& resource, bool isMapped = false);

    void freeAll();
};
