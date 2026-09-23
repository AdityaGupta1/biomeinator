// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include <cstdint>
#include <vector>

struct DirtyRange
{
    uint32_t begin{ 0 };
    uint32_t end{ 0 }; // exclusive

    bool operator==(const DirtyRange&) const = default;
};

class DirtyRangeSet
{
private:
    std::vector<DirtyRange> ranges;

public:
    bool insert(uint32_t begin, uint32_t end);
    void clear();

    bool empty() const;
    const std::vector<DirtyRange>& getRanges() const;
    bool validateInvariants() const;
};
