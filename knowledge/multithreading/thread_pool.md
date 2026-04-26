_Last edited: 2026-04-26_

# Thread Pool

`src/multithreading/thread_pool.h/cpp` — simple mutex+condvar thread pool used exclusively by the terrain system.

## Task Model

Tasks are `{function pointer, Chunk*}` pairs — not general-purpose lambdas. Every task operates on a chunk and receives the worker's `ThreadMemoryAllocator` as a second argument. This fixed signature avoids heap allocation per task.

## Local Task Batching

Each worker dequeues up to `MAX_NUM_LOCAL_TASKS` (8) tasks while holding the lock, then processes them all outside the lock. This reduces lock contention when many tasks are queued simultaneously (the terrain manager bulk-enqueues up to 48 per frame).

## Notify Strategy

`bulkEnqueue` uses `notify_all()` when enqueuing multiple tasks but `notify_one()` for a single task. This avoids thundering-herd wakeups for single-task enqueues while still waking all workers for batch submissions.

## Lifetime

The pool is initialized with `hardware_concurrency - 1` workers (leaving one core for the main thread). `shutdown()` sets the stop flag, drains the queue without executing remaining tasks, and joins all threads. Tasks in flight will complete before join returns, but queued-but-not-started tasks are discarded.
