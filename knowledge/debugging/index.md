_Last edited: 2026-08-16_

# Debugging Knowledgebase

Tooling and technique for diagnosing GPU-side faults — the cases where the debug layer and a
call stack are not enough because the failure happens on the GPU, asynchronously, long after
the offending command was recorded.

Entries here may carry a `.patch` alongside them. Instrumentation too invasive to keep in the
tree, but too laborious to work out from scratch again, is recorded as a reference diff.
Those are snapshots to read and reimplement from, not patches to apply — they go stale as the
surrounding code moves.

| Entry | Description |
|---|---|
| [aftermath.md](aftermath.md) | Nsight Aftermath crash dumps, pass markers and resource VA tracking; why over DRED, and how to read a dump |
