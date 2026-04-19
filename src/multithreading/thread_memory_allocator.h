// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "debug.h"

#include <vector>

class ThreadMemoryAllocator
{
private:
    uint8_t* data{ nullptr };
    size_t sizeBytes{ 0 };
    size_t offsetBytes{ 0 };

    std::vector<uint8_t*> toFree{};

    void allocate(size_t sizeBytes);
    void resize(size_t newSizeBytes);

    size_t getAlignedOffsetBytes(size_t alignment) const
    {
        const size_t alignedOffsetBytes = (this->offsetBytes + (alignment - 1)) & ~static_cast<size_t>(alignment - 1);
        return alignedOffsetBytes;
    }

public:
    ThreadMemoryAllocator();
    ~ThreadMemoryAllocator();

    template<class T>
    T* request(size_t numElements)
    {
        static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__, "ThreadMemoryAllocator does not support over-aligned types");
        ASSERT(numElements > 0);

        size_t alignedOffsetBytes = getAlignedOffsetBytes(alignof(T));

        const size_t allocBytes = numElements * sizeof(T);
        const size_t neededSizeBytes = alignedOffsetBytes + allocBytes;
        if (neededSizeBytes > this->sizeBytes)
        {
            size_t newSizeBytes = this->sizeBytes * 2;
            while (newSizeBytes < neededSizeBytes)
            {
                newSizeBytes *= 2;
            }
            this->resize(newSizeBytes);
            alignedOffsetBytes = 0;
        }

        T* result = reinterpret_cast<T*>(this->data + alignedOffsetBytes);
        this->offsetBytes = alignedOffsetBytes + allocBytes;
        return result;
    }

    void clear();
};
