# Known Bugs / Open Issues

## Export race: chunk at exactly `HAS_TERRAIN` may be exported mid-mutation

### Setup

`exportWorld()` runs on the main/window thread (Ctrl+U handler). Terrain tasks run on the thread pool concurrently.

### Mutation surface

For a chunk A:
- `generateTerrain` writes A's `blocks`, `biomes`, `structures`, then `advanceState(HAS_TERRAIN)`.
- `fillStructuresAndDecorators` reads neighbor `structures` (immutable past `HAS_TERRAIN`), writes A's `blocks` only (cross-chunk structure overlap from neighbors' structure lists, plus own decorator pass), then `advanceState(HAS_ALL_BLOCKS)`.

No neighbor task ever writes to A's data. Only A's own task mutates A.

### Race

Exporter gates on `state >= HAS_TERRAIN`. State only advances (monotonic), so:

- Gate observes `>= HAS_ALL_BLOCKS` → fillStructures finished, A's blocks/biomes/structures all frozen. **Race-free.**
- Gate observes exactly `HAS_TERRAIN` → A may be at `HAS_TERRAIN` (frozen) or may transition to `FILLING_STRUCTURES` between gate and read. In the latter case, exporter reads A's `blocks` while A's own fillStructures is writing them. **Torn / partial-state read.**

### Severity

Low. Reasons:
1. Ctrl+U is manually triggered, typically when terrain looks settled (no active gen).
2. `Block` is `uint16_t` — torn reads on x86 are unlikely but technically UB.
3. Exported `HAS_TERRAIN` chunks are boundary chunks by design. On import, they re-run `fillStructuresAndDecorators` from scratch, which overwrites the racy `blocks` content. So divergence in the captured snapshot gets clobbered on import — net effect is benign even when race fires.

### Possible fixes (if needed later)

- Tighten gate to `state >= HAS_ALL_BLOCKS` — drops boundary chunks from export. Loses some content at world edge.
- Quiesce the thread pool before export (drain in-flight tasks, block dispatch) — clean but more plumbing.
- Add a per-chunk export-snapshot lock — too heavy for a debug feature.

Not fixing now. Documented for future reference.
