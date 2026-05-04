_Last edited: 2026-05-03_

# Known issue: data race on import-tracking globals

## What's racy

`terrain.cpp` has three file-static globals used to track "has the imported
world finished its initial BLAS builds":

```cpp
static uint32_t expectedBlasBuildChunks{ 0 };
static uint32_t completedBlasBuildChunks{ 0 };
static bool worldImportActive{ false };
```

Access pattern:

| Site | Thread | Op |
|---|---|---|
| `addChunkToCreateBlas` (post-`task_generateGeometry`) | worker | reads `worldImportActive`, `++completedBlasBuildChunks` (under `chunksToCreateBlasMutex`) |
| `isImportComplete` (called from `Renderer::render`) | main | reads `worldImportActive`, reads `completedBlasBuildChunks`, writes `worldImportActive = false` |
| `importWorldImpl` / `reimportWorld` | main | initializes all three |

Reads on the main thread are unsynchronised with the worker writes — strictly
UB under the C++ memory model. In practice on x86 the loads see eventually
consistent values and the only observable failure mode would be the
"fully loaded" log line being delayed by a frame.

## Why we're leaving it

The "right" fix is to make these `std::atomic`, but:

- `worldImportActive` is read by every `addChunkToCreateBlas` call — putting
  it under the existing `chunksToCreateBlasMutex` already serialises the
  write side, so workers see a consistent value during the short window when
  it matters (initial import). Promoting to `std::atomic<bool>` would add a
  second synchronisation site for no real gain.
- `completedBlasBuildChunks` only matters until it crosses
  `expectedBlasBuildChunks`. Once `worldImportActive` flips false (one-shot),
  the counter is dead.
- The whole tracking system is a one-shot test-mode aid (gate the
  accumulation reset until import-time BLAS churn settles). Any race
  manifests as "screenshot taken one frame too early", which would just fail
  the golden compare loudly — not a silent corruption.

## What would actually break this

If `addChunkToCreateBlas` is ever called *after* `worldImportActive` flips
false on the main thread, the worker's stale read of `worldImportActive ==
true` would cause an extra `++completedBlasBuildChunks`. Harmless because
`isImportComplete` no longer consumes the counter past that point.

The order is also one-way: `worldImportActive` only ever transitions
`false → true` (in `importWorldImpl`) → `false` (one-shot in
`isImportComplete`). It never re-arms within a session.

## Resolution

Acknowledged. No fix planned. If the import-tracking system grows beyond a
test-mode gate (e.g. driving gameplay logic, not just a screenshot trigger),
revisit and convert to atomics.
