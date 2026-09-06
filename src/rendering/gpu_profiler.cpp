// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "gpu_profiler.h"

#include "debug.h"
#include "logger.h"
#include "rendering/buffer/buffer_helper.h"
#include "rendering/dxr_common.h"
#include "rendering/renderer.h"

#include <pix3.h>

namespace GpuProfiler
{

// Two timestamps per scope plus two for the frame itself
static constexpr uint32_t MAX_QUERIES_PER_SLOT = 128;
static constexpr uint32_t NUM_SLOTS = Renderer::NUM_FRAMES_IN_FLIGHT;

struct ScopeRecord
{
    const char* name;
    uint32_t depth;
    uint32_t beginQueryIdx;
    uint32_t endQueryIdx;
};

struct Slot
{
    uint32_t frameNumber{ 0 };
    uint32_t numQueries{ 0 };
    bool pending{ false };
    std::vector<ScopeRecord> scopes;
};

static ComPtr<ID3D12QueryHeap> queryHeap;
static ComPtr<ID3D12Resource> readbackBuffer;
static double msPerTick{ 0.0 };

static Slot slots[NUM_SLOTS];
static uint32_t currentSlotIdx{ 0 };
static std::vector<uint32_t> openScopes; // indices into the current slot's scopes
static bool warnedOverflow{ false };

void init()
{
    const D3D12_QUERY_HEAP_DESC heapDesc = {
        .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
        .Count = NUM_SLOTS * MAX_QUERIES_PER_SLOT,
    };
    CHECK_HRESULT(Renderer::getDevice()->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&queryHeap)));
    queryHeap->SetName(L"gpuProfiler queryHeap");

    readbackBuffer =
        BufferHelper::createBasicBuffer(NUM_SLOTS * MAX_QUERIES_PER_SLOT * sizeof(uint64_t), &READBACK_HEAP);
    readbackBuffer->SetName(L"gpuProfiler readback");

    uint64_t ticksPerSecond = 0;
    CHECK_HRESULT(Renderer::getGraphicsQueue()->GetTimestampFrequency(&ticksPerSecond));
    msPerTick = 1000.0 / static_cast<double>(ticksPerSecond);
}

void destroy()
{
    queryHeap.Reset();
    readbackBuffer.Reset();
}

static uint32_t slotQueryBase(const uint32_t slotIdx)
{
    return slotIdx * MAX_QUERIES_PER_SLOT;
}

bool collect(const uint32_t slotIdx, FrameTimings& outTimings)
{
    Slot& slot = slots[slotIdx];
    if (!slot.pending)
    {
        return false;
    }
    slot.pending = false;

    const size_t byteOffset = slotQueryBase(slotIdx) * sizeof(uint64_t);
    const D3D12_RANGE readRange = { byteOffset, byteOffset + slot.numQueries * sizeof(uint64_t) };
    uint8_t* mapped = nullptr;
    CHECK_HRESULT(readbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
    const uint64_t* ticks = reinterpret_cast<const uint64_t*>(mapped + byteOffset);

    outTimings.frameNumber = slot.frameNumber;
    outTimings.totalMs = static_cast<double>(ticks[slot.numQueries - 1] - ticks[0]) * msPerTick;
    outTimings.scopes.clear();
    outTimings.scopes.reserve(slot.scopes.size());
    for (const ScopeRecord& scope : slot.scopes)
    {
        if (scope.endQueryIdx == ~0u)
        {
            continue;
        }
        outTimings.scopes.push_back({
            .name = scope.name,
            .depth = scope.depth,
            .ms = static_cast<double>(ticks[scope.endQueryIdx] - ticks[scope.beginQueryIdx]) * msPerTick,
        });
    }

    const D3D12_RANGE emptyRange = { 0, 0 };
    readbackBuffer->Unmap(0, &emptyRange);
    return true;
}

// Returns the query index written, or ~0u if the slot is full
static uint32_t writeTimestamp(ID3D12GraphicsCommandList* cmdList)
{
    Slot& slot = slots[currentSlotIdx];
    if (slot.numQueries >= MAX_QUERIES_PER_SLOT)
    {
        if (!warnedOverflow)
        {
            Logger::logWarning("GpuProfiler: more than %u timestamps in a frame, later scopes are dropped",
                               MAX_QUERIES_PER_SLOT);
            warnedOverflow = true;
        }
        return ~0u;
    }

    const uint32_t queryIdx = slot.numQueries++;
    cmdList->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotQueryBase(currentSlotIdx) + queryIdx);
    return queryIdx;
}

void beginFrame(ID3D12GraphicsCommandList* cmdList, const uint32_t slotIdx, const uint32_t frameNumber)
{
    ASSERT(openScopes.empty(), "GpuProfiler scope left open across frames");
    ASSERT(!slots[slotIdx].pending, "GpuProfiler slot reused before its timings were collected");

    currentSlotIdx = slotIdx;
    Slot& slot = slots[slotIdx];
    slot.frameNumber = frameNumber;
    slot.numQueries = 0;
    slot.scopes.clear();

    writeTimestamp(cmdList);
}

void endFrame(ID3D12GraphicsCommandList* cmdList)
{
    ASSERT(openScopes.empty(), "GpuProfiler scope left open at end of frame");

    Slot& slot = slots[currentSlotIdx];
    writeTimestamp(cmdList);

    const uint32_t queryBase = slotQueryBase(currentSlotIdx);
    cmdList->ResolveQueryData(queryHeap.Get(),
                              D3D12_QUERY_TYPE_TIMESTAMP,
                              queryBase,
                              slot.numQueries,
                              readbackBuffer.Get(),
                              queryBase * sizeof(uint64_t));
    slot.pending = true;
}

void beginScope(ID3D12GraphicsCommandList* cmdList, const char* name)
{
    Slot& slot = slots[currentSlotIdx];
    const uint32_t depth = static_cast<uint32_t>(openScopes.size());
    PIXBeginEvent(cmdList, PIX_COLOR_INDEX(static_cast<BYTE>(depth)), name);

    const uint32_t beginQueryIdx = writeTimestamp(cmdList);
    openScopes.push_back(beginQueryIdx == ~0u ? ~0u : static_cast<uint32_t>(slot.scopes.size()));
    if (beginQueryIdx != ~0u)
    {
        slot.scopes.push_back({ .name = name, .depth = depth, .beginQueryIdx = beginQueryIdx, .endQueryIdx = ~0u });
    }
}

void endScope(ID3D12GraphicsCommandList* cmdList)
{
    ASSERT(!openScopes.empty(), "GpuProfiler endScope without beginScope");
    const uint32_t scopeIdx = openScopes.back();
    openScopes.pop_back();

    if (scopeIdx != ~0u)
    {
        // Stays ~0u if the slot is full; collect() skips such scopes
        slots[currentSlotIdx].scopes[scopeIdx].endQueryIdx = writeTimestamp(cmdList);
    }

    PIXEndEvent(cmdList);
}

ScopedZone::ScopedZone(ID3D12GraphicsCommandList* cmdList, const char* name) : cmdList(cmdList)
{
    beginScope(cmdList, name);
}

ScopedZone::~ScopedZone()
{
    endScope(this->cmdList);
}

} // namespace GpuProfiler
