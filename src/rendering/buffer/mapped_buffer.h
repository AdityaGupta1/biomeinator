#pragma once

#include "rendering/dxr_includes.h"

class MappedBuffer
{
private:
    void* host_buffer{ nullptr };
    ComPtr<ID3D12Resource> dev_buffer{ nullptr };
    uint64_t bufferSizeBytes{ 0 };

public:
    void resize(uint64_t size, ToFreeList* toFreeList);
};
