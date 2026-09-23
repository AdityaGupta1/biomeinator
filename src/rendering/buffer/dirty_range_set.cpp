// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "dirty_range_set.h"

#include <algorithm>
#include <iterator>

bool DirtyRangeSet::insert(uint32_t begin, uint32_t end)
{
    if (begin >= end)
    {
        return false;
    }
    if (this->ranges.empty())
    {
        this->ranges.emplace_back(begin, end);
        return true;
    }

    DirtyRange& last = this->ranges.back();
    if (begin >= last.end)
    {
        if (begin == last.end)
        {
            last.end = end;
        }
        else
        {
            this->ranges.push_back({ begin, end });
        }
        return true;
    }

    auto it = std::lower_bound(this->ranges.begin(),
                               this->ranges.end(),
                               begin,
                               [](const DirtyRange& range, uint32_t value) { return range.end < value; });
    if (it == this->ranges.end() || it->begin > end)
    {
        this->ranges.insert(it, { begin, end });
        return true;
    }

    it->begin = std::min(it->begin, begin);
    it->end = std::max(it->end, end);
    auto next = std::next(it);
    while (next != this->ranges.end() && next->begin <= it->end)
    {
        it->end = std::max(it->end, next->end);
        next = this->ranges.erase(next);
    }
    return true;
}

void DirtyRangeSet::clear()
{
    this->ranges.clear();
}

bool DirtyRangeSet::empty() const
{
    return this->ranges.empty();
}

const std::vector<DirtyRange>& DirtyRangeSet::getRanges() const
{
    return this->ranges;
}

bool DirtyRangeSet::validateInvariants() const
{
    uint32_t previousEnd = 0;
    bool isFirst = true;
    for (const DirtyRange& range : this->ranges)
    {
        if (range.begin >= range.end || (!isFirst && range.begin <= previousEnd))
        {
            return false;
        }
        previousEnd = range.end;
        isFirst = false;
    }
    return true;
}
