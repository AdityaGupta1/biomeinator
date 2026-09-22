// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "cpu_profiler.h"

#include "debug.h"

#include <chrono>
#include <cstring>

namespace CpuProfiler
{

namespace
{

using Clock = std::chrono::steady_clock;

struct OpenScope
{
    uint32_t timingIdx;
    Clock::time_point start;
};

bool enabled = false;
std::vector<ScopeTiming> timings;
std::vector<OpenScope> openScopes;

} // namespace

void init(const bool enable)
{
    enabled = enable;
}

void beginFrame()
{
    ASSERT(openScopes.empty(), "CpuProfiler scope left open across frames");
    timings.clear();
}

const std::vector<ScopeTiming>& endFrame()
{
    ASSERT(openScopes.empty(), "CpuProfiler scope left open at end of frame");
    return timings;
}

void beginScope(const char* const name)
{
    if (!enabled)
    {
        return;
    }

    const uint32_t depth = static_cast<uint32_t>(openScopes.size());
    uint32_t timingIdx = static_cast<uint32_t>(timings.size());
    for (uint32_t i = 0; i < timings.size(); ++i)
    {
        if (timings[i].depth == depth && std::strcmp(timings[i].name, name) == 0)
        {
            timingIdx = i;
            break;
        }
    }
    if (timingIdx == timings.size())
    {
        timings.push_back({ .name = name, .depth = depth, .ms = 0.0 });
    }
    openScopes.push_back({ .timingIdx = timingIdx, .start = Clock::now() });
}

void endScope()
{
    if (!enabled)
    {
        return;
    }

    ASSERT(!openScopes.empty(), "CpuProfiler endScope without beginScope");
    const OpenScope scope = openScopes.back();
    openScopes.pop_back();
    timings[scope.timingIdx].ms += std::chrono::duration<double, std::milli>(Clock::now() - scope.start).count();
}

ProfileScope::ProfileScope(const char* const name)
{
    beginScope(name);
}

ProfileScope::~ProfileScope()
{
    endScope();
}

} // namespace CpuProfiler
