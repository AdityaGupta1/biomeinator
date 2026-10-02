# CPU chunk eviction and region streaming

_Created: 2026-09-22_

## Objective and agreed scope

Bound CPU terrain memory as the camera explores the world. Stop scheduling work
for distant chunks, keep them in a per-region staging state while outstanding
work drains, then write sufficiently distant regions to disk and release their
chunk data. Load regions again before nearby generation or rendering needs them.

Use hysteresis: load within an inner threshold and evict beyond a larger outer
threshold, retaining the current residency state between the two. Distance must
be measured against region bounds and include the generation dependency margin.
Discard unfinished chunks once their outstanding work and references drain;
regenerate them if needed later instead of serializing intermediate generation.

The cache is disposable session storage under the Windows temporary directory:
`biomeinator/<unique-world-ID>/`. An OS-held file lock is authoritative for whether
a world is in use; a readable status file may accompany it. At renderer startup,
delete abandoned session directories whose locks can be acquired. Coordinate
creation and cleanup across concurrent renderer processes. Stop workers and I/O
before releasing a world's lock on shutdown or world replacement.

There is no requirement to recover cached worlds after app restarts or migrate
cache data between builds. Explicit export/import remains the durable world path.
Existing v5/v6 exports are golden-image fixtures, not worlds intended for exploring
past their boundaries; approximations for their missing generation data are
acceptable.

## Part 1: Complete, reusable region serialization (implement first)

- Save final blocks, biomes, sparse block states, surface-structure candidates,
  cave-structure candidates (including available height), and the original
  pre-structure terrain air and solid-cube masks in a new region format version.
- Preserve candidate order because structure fill order affects final blocks.
- Load exact saved masks without rebuilding them from decorated blocks.
- Keep v5/v6 readers and their existing approximations: reconstruct masks from
  loaded blocks, leave missing cave candidates empty, and migrate v5 block states.
- Extract region serialization and I/O from whole-world operations so future
  streaming can reuse it without changing the camera, seed, or global residency.
- Serialize only chunks with completed block generation. Derived segments, meshes,
  neighbor pointers, readiness counters, and deferred generation scratch are not
  part of the format.
- Check write/close failures, publish completed files safely, validate payloads,
  and decode a complete region before attaching any of its chunks to the world.
- Validate exact round trips, generation across imported/fresh boundaries,
  malformed/truncated files, and compatibility with existing golden worlds.

This part does not implement eviction, scheduling changes, a temporary-world
lifecycle, or asynchronous cache I/O.

## Part 2: Lifetimes that allow removal and reattachment

- Track queued/running tasks and every neighboring region they access, retaining
  those regions until the task actually returns (state publication is earlier).
- Make completion/revisit queues safe through ownership or coordinate plus
  lifetime identifiers so stale events cannot access removed chunks.
- Replace the assumption that readiness notifications happen exactly once over
  permanent neighbor membership. Readiness must work when old neighbors survive
  and other chunks disappear or return.
- Add inactive/staged residency without moving live chunk objects prematurely.
- Detach or rebuild region links, cardinal chunk links, and cached structure
  neighborhoods only when readers are safe.
- Discard unfinished chunks after outstanding work and references drain.
- Exercise removal/recreation beside surviving chunks before adding disk I/O.

## Part 3: Temporary disk cache and distance policy

- Create unique session directories and hold the in-use lock for their lifetime.
  Clean abandoned sessions at startup and retire old sessions during reimport.
- Add bounded background region writes and loads, with the hysteresis policy
  above. Keep the RAM copy until a write succeeds; bound staging and I/O memory so
  slow storage cannot produce an unbounded eviction backlog.
- Do not evict regions needed by queued/running work or active dependencies.
- Reserve loading regions so normal chunk creation cannot regenerate the same
  coordinates while their file is being read. Integrate decoded data on the main
  thread and handle returning while a write/load is in progress.
- Include cached regions in manual exports. Preserve import behavior without
  resetting camera or seed during individual cache reloads.
- Test long travel and return (memory plateau and identical blocks), rapid
  reversals, negative coordinates, boundary structures/decorators, write failures,
  crash cleanup, and multiple renderer processes.

## Important existing constraints

Chunks currently retain their blocks and masks forever; leaving geometry range
only destroys render instances. A generated chunk uses 256 KiB for blocks and
32 KiB for its two masks, before other data (288 MiB per full 32x32 region).

New stage scheduling is already distance-gated, but queued tasks are not canceled.
Tasks and completion queues hold raw chunk pointers, tasks read neighbors, and
readiness counters assume chunks never disappear. Reusing full-world import for
incremental loading would violate these assumptions.

Both terrain masks must describe pre-structure terrain. Rebuilding them from final
blocks changes neighboring lamp growth and cave-decorator support. Cave candidate
lists must survive because their structures can extend into newly generated
neighbors. Existing exports omit these inputs; their approximations remain a
legacy compatibility path, not the behavior for newly exported regions.
