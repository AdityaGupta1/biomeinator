// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include <cstddef>
#include <map>
#include <optional>

struct FreeRange
{
    size_t offsetBytes{ 0 };
    size_t sizeBytes{ 0 };

    bool operator==(const FreeRange&) const = default;
};

class FreeRangeAllocator
{
private:
    struct FreeNode;
    using OffsetMap = std::map<size_t, FreeNode>;
    using OffsetIter = OffsetMap::iterator;
    using SizeMap = std::multimap<size_t, OffsetIter>;
    using SizeIter = SizeMap::iterator;

    struct FreeNode
    {
        size_t sizeBytes;
        SizeIter sizeIter;
    };

    const size_t alignmentBytes;
    size_t capacityBytes{ 0 };
    OffsetMap freeByOffset;
    SizeMap freeBySize;

    void insertFreeNode(size_t offsetBytes, size_t sizeBytes);
    void eraseFreeNode(OffsetIter offsetIter);

public:
    explicit FreeRangeAllocator(size_t alignmentBytes = 0);

    void reset(size_t newCapacityBytes);
    bool grow(size_t newCapacityBytes);

    std::optional<FreeRange> allocate(size_t requestedSizeBytes);
    bool release(FreeRange range);

    size_t getAllocationSize(size_t requestedSizeBytes) const;
    size_t getCapacityBytes() const;
    size_t getFreeBytes() const;
    size_t getFreeTailBytes() const;
    bool isCompletelyFree() const;
    bool validateInvariants() const;
};
