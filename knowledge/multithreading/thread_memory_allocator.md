_Last edited: 2026-09-22_

# Thread Memory Allocator

`src/multithreading/thread_memory_allocator.h/cpp` — per-thread linear (bump) allocator for scratch memory during chunk generation tasks.

## Why It Exists

Chunk generation needs multiple temporary arrays (noise buffers, heightfields, segment classification) that are large but short-lived. Standard `new`/`malloc` per task would fragment the heap and contend on the global allocator lock. Instead, each worker thread owns one `ThreadMemoryAllocator` that bump-allocates from a single buffer and resets after each task.

## Growth Strategy

Starts at 64 KB. When a `request()` exceeds remaining capacity, the buffer doubles (repeatedly if needed). The old buffer is stashed in `toFree` — it's not freed immediately because earlier pointers from this task still reference it. All stashed buffers are freed on `clear()`.

This means within a single task, multiple backing buffers can coexist. Pointers returned by `request()` remain valid until `clear()`.

Unit coverage deliberately writes through allocations on both sides of repeated growth. A
size-only test would miss the allocator's most important promise: growing must not invalidate
scratch pointers that the current task already holds.

## Alignment

`request<T>()` aligns the bump pointer to `alignof(T)` before allocating. Over-aligned types (alignment > default new alignment) are statically rejected.

## Reset Cadence

`clear()` is called by the thread pool worker after each task completes. This means all scratch memory from a task is freed before the next task starts — no accumulation across tasks.
The tests also alternate growth-heavy and small allocation cycles to verify that clearing
drops retired buffers and restarts the surviving current buffer at offset zero.
