#pragma once

#include "dxr_common.h"

#include <list>

struct ManagedBufferSection
{
    uint64_t offset;
    uint64_t size;
};

class ManagedBuffer
{
private:
    ComPtr<ID3D12Resource> dev_buffer{ nullptr };
    uint64_t bufferSize{ 0 };

    void* host_uploadBuffer{ nullptr };
    ComPtr<ID3D12Resource> dev_uploadBuffer{ nullptr };
    uint64_t uploadBufferSize{ 0 };
    uint64_t uploadBufferOffset{ 0 };

    std::list<ManagedBufferSection> freeList;

public:
    void init(uint64_t bufferSize, uint64_t uploadBufferSize);

    ManagedBufferSection upload(ID3D12GraphicsCommandList* cmdList, const void* data, uint64_t size);
    void free(ManagedBufferSection section);

    ID3D12Resource* getBuffer();
};
