/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2026 Aditya Gupta

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

    size_t getAlignedOffsetBytes(size_t alignment)
    {
        const size_t alignedOffsetBytes = (this->offsetBytes + (alignment - 1)) & ~static_cast<size_t>(alignment - 1);
        return alignedOffsetBytes;
    }

public:
    ThreadMemoryAllocator();
    ~ThreadMemoryAllocator();

    template<class T> T* request(size_t numElements)
    {
        static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
                      "ThreadMemoryAllocator does not support over-aligned types");
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
