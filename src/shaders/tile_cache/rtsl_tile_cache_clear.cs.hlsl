// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

cbuffer ClearConstants : REGISTER_B(RTSL_CACHE_CLEAR, CONSTANTS)
{
    uint numSlots;          // total slots in the target buffer
    uint strideThreads;     // total threads launched this dispatch
    uint targetUavHeapIdx;  // bindless index of the buffer to clear
    uint pad0;              // pad to 16 bytes (root sig uploads only the 3 above)
};

[shader("compute")]
[numthreads(RTSL_TILE_CACHE_CLEAR_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWByteAddressBuffer cache = ResourceDescriptorHeap[targetUavHeapIdx];

    // Grid-stride clear. A thread's slots are strideThreads apart, so on each
    // iteration consecutive lanes hit consecutive slots — a fully coalesced
    // burst. (A block partition where one thread owns SLOTS_PER_THREAD adjacent
    // slots would scatter each warp across strideThreads-spaced addresses.)
    const uint tid = dispatchThreadId.x;
    [unroll]
    for (uint i = 0u; i < RTSL_TILE_CACHE_CLEAR_SLOTS_PER_THREAD; ++i)
    {
        const uint slot = tid + i * strideThreads;
        if (slot < numSlots)
        {
            const uint byteOffset = slot * RTSL_TILE_CACHE_SLOT_BYTES;
            cache.Store(byteOffset, LIGHT_IDX_INVALID);  // lightIdx
            cache.Store(byteOffset + 4u, 0u);            // normalTag
            cache.Store(byteOffset + 8u, 0u);            // attempts
            cache.Store(byteOffset + 12u, 0u);           // successes
        }
    }
}
