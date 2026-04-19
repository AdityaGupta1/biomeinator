// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

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
