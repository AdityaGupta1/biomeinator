// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "descriptor_index_allocator.h"

#include <algorithm>

void DescriptorIndexAllocator::reset(uint32_t newCapacity,
                                     uintptr_t newCpuStart,
                                     uint64_t newGpuStart,
                                     uint32_t newHandleIncrement)
{
    this->capacity = newCapacity;
    this->cpuStart = newCpuStart;
    this->gpuStart = newGpuStart;
    this->handleIncrement = newHandleIncrement;
    this->freeIndices.clear();
    this->freeIndices.reserve(newCapacity);
    this->isFree.assign(newCapacity, 1);
    for (uint32_t index = newCapacity; index > 0; --index)
    {
        this->freeIndices.push_back(index - 1);
    }
}

std::optional<DescriptorIndexAllocation> DescriptorIndexAllocator::allocate()
{
    if (this->freeIndices.empty())
    {
        return std::nullopt;
    }

    const uint32_t index = this->freeIndices.back();
    this->freeIndices.pop_back();
    if (index >= this->capacity || !this->isFree[index])
    {
        return std::nullopt;
    }
    this->isFree[index] = 0;

    return DescriptorIndexAllocation{
        .index = index,
        .cpuHandle = this->cpuStart + static_cast<uintptr_t>(index) * this->handleIncrement,
        .gpuHandle = this->gpuStart + static_cast<uint64_t>(index) * this->handleIncrement,
    };
}

bool DescriptorIndexAllocator::release(uint32_t index)
{
    if (index >= this->capacity || this->isFree[index])
    {
        return false;
    }

    this->isFree[index] = 1;
    this->freeIndices.push_back(index);
    return true;
}

bool DescriptorIndexAllocator::release(uintptr_t cpuHandle, uint64_t gpuHandle)
{
    const std::optional<uint32_t> cpuIndex = this->indexFromCpuHandle(cpuHandle);
    const std::optional<uint32_t> gpuIndex = this->indexFromGpuHandle(gpuHandle);
    if (!cpuIndex || !gpuIndex || *cpuIndex != *gpuIndex)
    {
        return false;
    }
    return this->release(*cpuIndex);
}

std::optional<uint32_t> DescriptorIndexAllocator::indexFromCpuHandle(uintptr_t handle) const
{
    if (this->handleIncrement == 0 || handle < this->cpuStart)
    {
        return std::nullopt;
    }
    const uintptr_t offset = handle - this->cpuStart;
    if (offset % this->handleIncrement != 0)
    {
        return std::nullopt;
    }
    const uintptr_t index = offset / this->handleIncrement;
    return index < this->capacity ? std::optional<uint32_t>(static_cast<uint32_t>(index)) : std::nullopt;
}

std::optional<uint32_t> DescriptorIndexAllocator::indexFromGpuHandle(uint64_t handle) const
{
    if (this->handleIncrement == 0 || handle < this->gpuStart)
    {
        return std::nullopt;
    }
    const uint64_t offset = handle - this->gpuStart;
    if (offset % this->handleIncrement != 0)
    {
        return std::nullopt;
    }
    const uint64_t index = offset / this->handleIncrement;
    return index < this->capacity ? std::optional<uint32_t>(static_cast<uint32_t>(index)) : std::nullopt;
}

uint32_t DescriptorIndexAllocator::getCapacity() const
{
    return this->capacity;
}

size_t DescriptorIndexAllocator::getFreeCount() const
{
    return this->freeIndices.size();
}

bool DescriptorIndexAllocator::validateInvariants() const
{
    if (this->isFree.size() != this->capacity || this->freeIndices.size() > this->capacity ||
        (this->capacity > 0 && this->handleIncrement == 0))
    {
        return false;
    }

    std::vector<uint8_t> seen(this->capacity, 0);
    for (const uint32_t index : this->freeIndices)
    {
        if (index >= this->capacity || seen[index] || !this->isFree[index])
        {
            return false;
        }
        seen[index] = 1;
    }

    return std::equal(seen.begin(), seen.end(), this->isFree.begin());
}
