#pragma once

#include "dxr_common.h"

#include <list>

struct ManagedBufferSection
{
    uint32_t byteOffset;
    uint32_t byteSize;
};

class ManagedBuffer
{
private:
    ComPtr<ID3D12Resource> dev_buffer{ nullptr };
    uint32_t bufferSize{ 0 };

    std::list<ManagedBufferSection> freeList;

public:
    void init(uint32_t byteSize);

    ManagedBufferSection copyFromUploadHeap(ID3D12GraphicsCommandList* cmdList,
                                            ID3D12Resource* dev_uploadBuffer,
                                            uint32_t byteSize);
    void free(ManagedBufferSection section);

    ID3D12Resource* getBuffer();
};
