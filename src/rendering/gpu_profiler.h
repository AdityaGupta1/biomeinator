// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "dxr_includes.h"

#include <vector>

// GPU timestamp scopes recorded into the frame command list, resolved once per frame and read
// back after that frame's fence. Every scope is also emitted as a PIX event so captures in
// Nsight or PIX carry the same names.
namespace GpuProfiler
{

struct ScopeTiming
{
    const char* name; // string literal; never owned
    uint32_t depth;
    double ms;
};

struct FrameTimings
{
    uint32_t frameNumber;
    double totalMs; // first to last timestamp of the frame's command list
    std::vector<ScopeTiming> scopes;
};

void init();
void destroy();

// Reads back the timings recorded into a frame slot, if any are pending. Only valid once the
// slot's fence has been waited on.
bool collect(uint32_t slotIdx, FrameTimings& outTimings);

// Recording for a frame is bracketed by beginFrame/endFrame; endFrame resolves the slot's
// queries into the readback buffer.
void beginFrame(ID3D12GraphicsCommandList* cmdList, uint32_t slotIdx, uint32_t frameNumber);
void endFrame(ID3D12GraphicsCommandList* cmdList);

void beginScope(ID3D12GraphicsCommandList* cmdList, const char* name);
void endScope(ID3D12GraphicsCommandList* cmdList);

class ScopedZone
{
public:
    ScopedZone(ID3D12GraphicsCommandList* cmdList, const char* name);
    ~ScopedZone();

    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;

private:
    ID3D12GraphicsCommandList* cmdList;
};

} // namespace GpuProfiler

#define GPU_PROFILER_CONCAT_IMPL(a, b) a##b
#define GPU_PROFILER_CONCAT(a, b) GPU_PROFILER_CONCAT_IMPL(a, b)
#define GPU_PROFILE_SCOPE(cmdList, name)                                                                               \
    const GpuProfiler::ScopedZone GPU_PROFILER_CONCAT(gpuProfileScope_, __LINE__)(cmdList, name)
