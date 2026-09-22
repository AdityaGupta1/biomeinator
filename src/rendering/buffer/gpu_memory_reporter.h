// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <string>
#include <vector>

struct GpuMemoryEntry
{
    std::string name;
    size_t allocatedBytes;
    size_t usedBytes; // allocatedBytes minus free-list space, for sub-allocated buffers
};

// Every live GPU buffer owner registers itself so a memory report can enumerate them all without
// each owner being plumbed through to the report
class GpuMemoryReporter
{
protected:
    GpuMemoryReporter();
    GpuMemoryReporter(const GpuMemoryReporter&);
    virtual ~GpuMemoryReporter();

public:
    virtual GpuMemoryEntry reportGpuMemory() const = 0;

    static std::vector<GpuMemoryEntry> collectAll();
};
