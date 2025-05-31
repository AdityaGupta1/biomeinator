#pragma once

#include "dxr_common.h"

#include <list>

struct ManagedBufferSection
{
    uint byteOffset;
    uint byteSize;
};

class ManagedBuffer
{
private:
    ComPtr<ID3D12Resource> dev_buffer{ nullptr };
    uint bufferSize{ 0 };

    std::list<ManagedBufferSection> freeList;

public:
    void init(uint byteSize);

    ManagedBufferSection copyFromUploadHeap(ID3D12GraphicsCommandList* cmdList,
                                            ID3D12Resource* dev_uploadBuffer,
                                            uint byteSize);
    void free(ManagedBufferSection section);

    ID3D12Resource* getBuffer();
};
