_Last edited: 2026-09-21_

# Thread Pool

`src/multithreading/thread_pool.h/cpp` — simple mutex+condvar thread pool used exclusively by the terrain system.

## Task Model

Tasks are `{function pointer, Chunk*}` pairs — not general-purpose lambdas. Every task operates on a chunk and receives the worker's `ThreadMemoryAllocator` as a second argument. This fixed signature avoids heap allocation per task.

## Local Task Batching

Each worker dequeues up to `MAX_NUM_LOCAL_TASKS` (8) tasks while holding the lock, then processes them all outside the lock. This reduces lock contention when many tasks are queued simultaneously (the terrain manager bulk-enqueues up to 48 per frame).

## Notify Strategy

`bulkEnqueue` uses `notify_all()` when enqueuing multiple tasks but `notify_one()` for a single task. This avoids thundering-herd wakeups for single-task enqueues while still waking all workers for batch submissions.

## Counters

The pool keeps two relaxed atomics for measurement rather than scheduling: summed busy
nanoseconds across workers (perf runs report worker utilization from it) and the number of
tasks queued or executing, which `Terrain::getStreamingStats` exposes so a perf run can tell
"nothing landed in the scene this frame" from "generation is finished".

## Priority

Workers run at `THREAD_PRIORITY_BELOW_NORMAL`. With one worker per core but one, a chunk
crossing that bulk-enqueues ~200 tasks wakes all of them at once, and at normal priority the
main thread lost its core for a scheduler quantum somewhere in that frame (measured as 3 to
10 ms holes in whatever scope was running, on top of the real work). Lower priority keeps the
render thread scheduled first; the workers still saturate the machine between frames. See
[tests → perf_runs.md](../tests/perf_runs.md#per-frame-timeline-and-chunk-crossings).

## Lifetime

The pool is initialized with `hardware_concurrency - 1` workers (leaving one core for the main thread). `shutdown()` sets the stop flag, drains the queue without executing remaining tasks, and joins all threads. Tasks in flight will complete before join returns, but queued-but-not-started tasks are discarded.
