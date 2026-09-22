// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "gpu_memory_reporter.h"

#include <set>

// Heap-allocated and never freed: reporters with static storage duration are destroyed after
// function-local statics, so a registry with a destructor would be gone by the time they unregister
static std::set<GpuMemoryReporter*>& liveReporters()
{
    static std::set<GpuMemoryReporter*>* const reporters = new std::set<GpuMemoryReporter*>();
    return *reporters;
}

GpuMemoryReporter::GpuMemoryReporter()
{
    liveReporters().insert(this);
}

GpuMemoryReporter::GpuMemoryReporter(const GpuMemoryReporter&)
{
    liveReporters().insert(this);
}

GpuMemoryReporter::~GpuMemoryReporter()
{
    liveReporters().erase(this);
}

std::vector<GpuMemoryEntry> GpuMemoryReporter::collectAll()
{
    std::vector<GpuMemoryEntry> entries;
    entries.reserve(liveReporters().size());
    for (const GpuMemoryReporter* const reporter : liveReporters())
    {
        entries.push_back(reporter->reportGpuMemory());
    }
    return entries;
}
