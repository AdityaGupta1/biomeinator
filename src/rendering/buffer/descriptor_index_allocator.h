// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

struct DescriptorIndexAllocation
{
    static constexpr uint32_t INVALID_INDEX = (std::numeric_limits<uint32_t>::max)();

    uint32_t index{ INVALID_INDEX };
    uintptr_t cpuHandle{ 0 };
    uint64_t gpuHandle{ 0 };

    bool operator==(const DescriptorIndexAllocation&) const = default;
};

class DescriptorIndexAllocator
{
private:
    uint32_t capacity{ 0 };
    uintptr_t cpuStart{ 0 };
    uint64_t gpuStart{ 0 };
    uint32_t handleIncrement{ 0 };
    std::vector<uint32_t> freeIndices;
    std::vector<uint8_t> isFree;

    std::optional<uint32_t> indexFromCpuHandle(uintptr_t handle) const;
    std::optional<uint32_t> indexFromGpuHandle(uint64_t handle) const;

public:
    void reset(uint32_t capacity, uintptr_t cpuStart, uint64_t gpuStart, uint32_t handleIncrement);

    std::optional<DescriptorIndexAllocation> allocate();
    bool release(uint32_t index);
    bool release(uintptr_t cpuHandle, uint64_t gpuHandle);

    uint32_t getCapacity() const;
    size_t getFreeCount() const;
    bool validateInvariants() const;
};
