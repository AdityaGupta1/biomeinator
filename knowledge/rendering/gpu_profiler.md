_Last edited: 2026-09-06_

# GPU Profiler

`src/rendering/gpu_profiler.h/.cpp` — timestamp-query scopes on the frame command list. Always
on; the per-frame cost is a few dozen `EndQuery` calls and one `ResolveQueryData`, which is why
there is no switch for it. Perf runs (see [tests → perf_runs.md](../tests/perf_runs.md)) are
the main consumer; the same data is available to anything else that wants it.

## Scope placement

`GPU_PROFILE_SCOPE(cmdList, "name")` brackets a block; `beginScope`/`endScope` exist for the
passes in `render()` whose setup and dispatch are not lexically nested. Scopes nest, and the
depth is recorded so reports can show the hierarchy.

The frame's own begin/end timestamps come from `beginFrame`/`endFrame`, so `totalMs` is the
whole command list, not the sum of scopes.

**Light tree build and radix sort are timed as one enclosing scope on purpose.** Their
dispatches run back-to-back without barriers between every one, so the GPU overlaps them and
per-dispatch timestamps inside the chain would attribute time to the wrong kernel. The same
reasoning applies to any future barrier-free chain: time the chain, not its links.

Every scope is also a PIX event with the same name, so Nsight and PIX captures show the same
regions the JSON reports. Markers compile to nothing in Release (see
[build → configs.md](../build/configs.md)).

## Readback timing

Results for a frame come back `NUM_FRAMES_IN_FLIGHT` frames later. Each frame context owns a
slot in the query heap and readback buffer, and `collect(slot)` is only valid after that
slot's fence has been waited on — which is exactly where `Renderer::beginFrame` calls it. A
slot flagged `pending` is never overwritten before it is collected; the assert in
`beginFrame` guards that. Draining the last in-flight slots needs a `flush()` first, which the
perf run does before writing its report.

Timings carry the frame number they were recorded on, so a consumer that only wants a window
of frames filters by number rather than by when the readback happened.

## Gotchas

- Scope names must be string literals; the profiler stores the pointer, never a copy.
- Timestamp frequency comes from the command queue and is cached at init; timestamps are
  meaningless across queues.
- `MAX_QUERIES_PER_SLOT` caps scopes per frame. Overflow drops later scopes and logs once
  rather than asserting, since a burst of BLAS builds should not take the app down.
