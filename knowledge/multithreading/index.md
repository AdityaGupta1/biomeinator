_Last edited: 2026-04-26_

# Multithreading Knowledgebase

Thread pool, parallel chunk generation pipeline, and per-thread memory management.

| Entry | Description |
|---|---|
| [thread_pool.md](thread_pool.md) | Work-stealing ThreadPool, worker count, task enqueue API |
| [chunk_gen_pipeline.md](chunk_gen_pipeline.md) | Five task types, dependency ordering, parallel generation flow |
| [thread_memory_allocator.md](thread_memory_allocator.md) | Per-thread linear scratch allocator, exponential growth, frame reset |
