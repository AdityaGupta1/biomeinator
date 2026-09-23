// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "free_range_allocator.h"

#include "debug.h"
#include "util/math.h"

#include <iterator>

FreeRangeAllocator::FreeRangeAllocator(size_t alignmentBytes) : alignmentBytes(alignmentBytes)
{
}

void FreeRangeAllocator::reset(size_t newCapacityBytes)
{
    this->capacityBytes = newCapacityBytes;
    this->freeByOffset.clear();
    this->freeBySize.clear();
    if (newCapacityBytes > 0)
    {
        this->insertFreeNode(0, newCapacityBytes);
    }
}

bool FreeRangeAllocator::grow(size_t newCapacityBytes)
{
    if (newCapacityBytes < this->capacityBytes)
    {
        return false;
    }
    if (newCapacityBytes == this->capacityBytes)
    {
        return true;
    }

    const size_t oldCapacityBytes = this->capacityBytes;
    const size_t addedSizeBytes = newCapacityBytes - oldCapacityBytes;
    if (!this->freeByOffset.empty())
    {
        const OffsetIter backIter = std::prev(this->freeByOffset.end());
        if (backIter->first + backIter->second.sizeBytes == oldCapacityBytes)
        {
            const size_t offsetBytes = backIter->first;
            const size_t sizeBytes = backIter->second.sizeBytes + addedSizeBytes;
            this->eraseFreeNode(backIter);
            this->insertFreeNode(offsetBytes, sizeBytes);
            this->capacityBytes = newCapacityBytes;
            return true;
        }
    }

    if (this->alignmentBytes != 0 && oldCapacityBytes % this->alignmentBytes != 0)
    {
        return false;
    }

    this->insertFreeNode(oldCapacityBytes, addedSizeBytes);
    this->capacityBytes = newCapacityBytes;
    return true;
}

void FreeRangeAllocator::insertFreeNode(size_t offsetBytes, size_t sizeBytes)
{
    const auto [offsetIter, inserted] = this->freeByOffset.insert({ offsetBytes, FreeNode{ sizeBytes, {} } });
    ASSERT(inserted, "freeByOffset already contains this offset");
    offsetIter->second.sizeIter = this->freeBySize.insert({ sizeBytes, offsetIter });
}

void FreeRangeAllocator::eraseFreeNode(OffsetIter offsetIter)
{
    this->freeBySize.erase(offsetIter->second.sizeIter);
    this->freeByOffset.erase(offsetIter);
}

std::optional<FreeRange> FreeRangeAllocator::allocate(size_t requestedSizeBytes)
{
    const size_t sizeBytes = this->getAllocationSize(requestedSizeBytes);
    if (sizeBytes == 0)
    {
        return std::nullopt;
    }

    const auto sizeIter = this->freeBySize.lower_bound(sizeBytes);
    if (sizeIter == this->freeBySize.end())
    {
        return std::nullopt;
    }

    const OffsetIter offsetIter = sizeIter->second;
    const size_t resultOffsetBytes = offsetIter->first;
    const size_t blockSizeBytes = offsetIter->second.sizeBytes;
    this->eraseFreeNode(offsetIter);

    if (blockSizeBytes > sizeBytes)
    {
        this->insertFreeNode(resultOffsetBytes + sizeBytes, blockSizeBytes - sizeBytes);
    }

    return FreeRange{ resultOffsetBytes, sizeBytes };
}

bool FreeRangeAllocator::release(FreeRange range)
{
    if (range.sizeBytes == 0 || range.offsetBytes > this->capacityBytes ||
        range.sizeBytes > this->capacityBytes - range.offsetBytes ||
        (this->alignmentBytes != 0 &&
         (range.offsetBytes % this->alignmentBytes != 0 || range.sizeBytes % this->alignmentBytes != 0)))
    {
        return false;
    }

    size_t mergedOffsetBytes = range.offsetBytes;
    size_t mergedSizeBytes = range.sizeBytes;
    const OffsetIter nextIter = this->freeByOffset.lower_bound(range.offsetBytes);

    OffsetIter prevIter = this->freeByOffset.end();
    if (nextIter != this->freeByOffset.begin())
    {
        prevIter = std::prev(nextIter);
        const size_t prevEndBytes = prevIter->first + prevIter->second.sizeBytes;
        if (prevEndBytes > range.offsetBytes)
        {
            return false;
        }
    }

    const size_t rangeEndBytes = range.offsetBytes + range.sizeBytes;
    if (nextIter != this->freeByOffset.end() && nextIter->first < rangeEndBytes)
    {
        return false;
    }

    if (prevIter != this->freeByOffset.end() && prevIter->first + prevIter->second.sizeBytes == range.offsetBytes)
    {
        mergedOffsetBytes = prevIter->first;
        mergedSizeBytes += prevIter->second.sizeBytes;
        this->eraseFreeNode(prevIter);
    }

    if (nextIter != this->freeByOffset.end() && mergedOffsetBytes + mergedSizeBytes == nextIter->first)
    {
        mergedSizeBytes += nextIter->second.sizeBytes;
        this->eraseFreeNode(nextIter);
    }

    this->insertFreeNode(mergedOffsetBytes, mergedSizeBytes);
    return true;
}

size_t FreeRangeAllocator::getAllocationSize(size_t requestedSizeBytes) const
{
    if (requestedSizeBytes == 0)
    {
        return 0;
    }
    return this->alignmentBytes == 0 ? requestedSizeBytes : MathUtil::roundUp(requestedSizeBytes, this->alignmentBytes);
}

size_t FreeRangeAllocator::getCapacityBytes() const
{
    return this->capacityBytes;
}

size_t FreeRangeAllocator::getFreeBytes() const
{
    size_t freeBytes = 0;
    for (const auto& [_, node] : this->freeByOffset)
    {
        freeBytes += node.sizeBytes;
    }
    return freeBytes;
}

size_t FreeRangeAllocator::getFreeTailBytes() const
{
    if (this->freeByOffset.empty())
    {
        return 0;
    }

    const auto backIter = std::prev(this->freeByOffset.end());
    return backIter->first + backIter->second.sizeBytes == this->capacityBytes ? backIter->second.sizeBytes : 0;
}

bool FreeRangeAllocator::isCompletelyFree() const
{
    if (this->capacityBytes == 0)
    {
        return this->freeByOffset.empty() && this->freeBySize.empty();
    }
    if (this->freeByOffset.size() != 1 || this->freeBySize.size() != 1)
    {
        return false;
    }
    const auto offsetIter = this->freeByOffset.begin();
    return offsetIter->first == 0 && offsetIter->second.sizeBytes == this->capacityBytes;
}

bool FreeRangeAllocator::validateInvariants() const
{
    if (this->freeByOffset.size() != this->freeBySize.size())
    {
        return false;
    }

    size_t totalFreeBytes = 0;
    size_t previousEndBytes = 0;
    bool isFirst = true;
    for (auto offsetIter = this->freeByOffset.begin(); offsetIter != this->freeByOffset.end(); ++offsetIter)
    {
        const size_t offsetBytes = offsetIter->first;
        const FreeNode& node = offsetIter->second;
        if (node.sizeBytes == 0 || offsetBytes > this->capacityBytes ||
            node.sizeBytes > this->capacityBytes - offsetBytes)
        {
            return false;
        }
        if (!isFirst && offsetBytes <= previousEndBytes)
        {
            return false;
        }
        const bool isTrailingRange = offsetBytes + node.sizeBytes == this->capacityBytes;
        if (this->alignmentBytes != 0 && (offsetBytes % this->alignmentBytes != 0 ||
                                          (!isTrailingRange && node.sizeBytes % this->alignmentBytes != 0)))
        {
            return false;
        }
        if (node.sizeIter == this->freeBySize.end() || node.sizeIter->first != node.sizeBytes ||
            node.sizeIter->second != offsetIter)
        {
            return false;
        }
        if (node.sizeBytes > this->capacityBytes - totalFreeBytes)
        {
            return false;
        }

        totalFreeBytes += node.sizeBytes;
        previousEndBytes = offsetBytes + node.sizeBytes;
        isFirst = false;
    }

    return true;
}
