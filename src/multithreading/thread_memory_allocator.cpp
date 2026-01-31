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

#include "thread_memory_allocator.h"

ThreadMemoryAllocator::ThreadMemoryAllocator()
{
    this->allocate(1 << 16 /*bytes*/);
}

ThreadMemoryAllocator::~ThreadMemoryAllocator()
{
    this->clear();
    delete[] this->data;
}

void ThreadMemoryAllocator::allocate(size_t sizeBytes)
{
    this->data = new uint8_t[sizeBytes];
    this->sizeBytes = sizeBytes;
    this->offsetBytes = 0;
}

void ThreadMemoryAllocator::resize(size_t newSizeBytes)
{
    this->toFree.push_back(this->data);
    this->allocate(newSizeBytes);
}

void ThreadMemoryAllocator::clear()
{
    for (uint8_t* ptrToFree : this->toFree)
    {
        delete[] ptrToFree;
    }
    this->toFree.clear();

    this->offsetBytes = 0;
}
