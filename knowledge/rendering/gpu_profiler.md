_Last edited: 2026-09-06_

# GPU Profiler

`src/rendering/gpu_profiler.h/.cpp` — timestamp-query scopes on the frame command list. The
scopes serve two purposes with different gating:

- **PIX events** are emitted from every scope whenever the build has markers at all (see
  [build → configs.md](../build/configs.md)), so interactive Nsight and PIX captures always
  carry the pass names.
- **Timestamps** are only recorded when `init` is called with them enabled, which the renderer
  does in perf mode only. Off, the query heap and readback buffer are never created and every
  profiler call reduces to a flag check, so an interactive run pays nothing beyond the markers.
  Perf runs (see [tests → perf_runs.md](../tests/perf_runs.md)) are the only consumer today;
  anything else wanting the data enables it the same way.

## Scope placement

`GPU_PROFILE_SCOPE(cmdList, "name")` brackets a block; `beginScope`/`endScope` exist for the
passes in `render()` whose setup and dispatch are not lexically nested. Scopes nest, and the
depth is recorded so reports can show the hierarchy.

The frame's own begin/end timestamps come from `beginFrame`/`endFrame`, so `totalMs` is the
whole command list, not the sum of scopes.

**Light tree build and radix sort are timed as one enclosing scope on purpose.** The sort
lives in the GPUSorting submodule and is not instrumented, and the build's individual
dispatches are short enough that per-dispatch numbers would be dominated by timestamp
granularity. If a scope is ever added around dispatches that have no barrier between them,
expect misattribution: the GPU overlaps such dispatches, so a timestamp between them does not
mark where one ends and the next begins.

## Readback timing

Results for a frame come back `NUM_FRAMES_IN_FLIGHT` frames later. Each frame context owns a
slot in the query heap and readback buffer, and `collect(slot)` is only valid after that
slot's fence has been waited on — which is exactly where `Renderer::beginFrame` calls it, via
`perfRunCollectTimings`. A slot flagged `pending` is never overwritten before it is collected;
the assert in `beginFrame` guards that. Draining the last in-flight slots needs a `flush()`
first, which the perf run does before writing its report.

Timings carry the frame number they were recorded on, so a consumer that only wants a window
of frames filters by number rather than by when the readback happened.

## Gotchas

- Scope names must be string literals; the profiler stores the pointer, never a copy.
- Timestamp frequency comes from the command queue and is cached at init; timestamps are
  meaningless across queues.
- `MAX_QUERIES_PER_SLOT` caps scopes per frame. Overflow drops later scopes and logs once
  rather than asserting, since a burst of BLAS builds should not take the app down.
