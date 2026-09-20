// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <cstdint>
#include <vector>

// Main-thread timing scopes, the CPU counterpart of GpuProfiler: perf runs report both so a
// frame spike can be attributed to the thread it came from. Only records when enabled at init
// (perf mode); otherwise every call is a flag check. Scopes nest and may repeat within a frame,
// in which case the frame's entry for that name sums them.
namespace CpuProfiler
{

struct ScopeTiming
{
    const char* name; // string literal; never owned
    uint32_t depth;
    double ms;
};

void init(bool enable);

void beginFrame();
// The scopes recorded since beginFrame, in order of first appearance
const std::vector<ScopeTiming>& endFrame();

void beginScope(const char* name);
void endScope();

class ProfileScope
{
public:
    explicit ProfileScope(const char* name);
    ~ProfileScope();

    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
};

} // namespace CpuProfiler

#define CPU_PROFILER_CONCAT_IMPL(a, b) a##b
#define CPU_PROFILER_CONCAT(a, b) CPU_PROFILER_CONCAT_IMPL(a, b)
#define CPU_PROFILE_SCOPE(name) const CpuProfiler::ProfileScope CPU_PROFILER_CONCAT(cpuProfileScope_, __LINE__)(name)
