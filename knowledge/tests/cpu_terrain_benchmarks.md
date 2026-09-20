_Last edited: 2026-09-20_

# Ad hoc CPU terrain experiments

Use a temporary harness when comparing terrain algorithms without worker scheduling or
rendering. [perf_runs.md](perf_runs.md) measures frames after world import, so it does not
measure CPU terrain generation. The useful comparison is the actual generation code on
identical chunks, with the same build settings and an explicit endpoint.

[cpu_terrain_benchmark_reference.patch](cpu_terrain_benchmark_reference.patch) preserves the
source of the September 2026 experiment. Like the
[Aftermath reference](../debugging/aftermath.md), **read it and reimplement against the current
tree; do not `git apply` it or run the archived scripts unchanged.** It is an investigation
snapshot, not a maintained benchmark target or a patch to keep synchronized with production.
Put a new experiment under an ignored `build/<experiment>/` directory. Production does not
need permanent timers, benchmark switches, or a new CMake target for this workflow.

## What is worth reusing

The snapshot contains the harness, synthetic fixtures, minimal engine substitutes, builder,
alternating runner, and exact-output comparison. The builder compiles the actual generator
and extracts actual Chunk mask/decorator method bodies; it also compiles the surface/cave
structure implementations. This avoids inventing a simplified terrain algorithm whose
performance or behavior differs from the application.

The private-member access helper and empty Chunk constructor let the harness prepare and
invoke those paths without starting a renderer, Region manager, or worker pool. Keep that
access inside the disposable harness. Re-read the
[chunk lifecycle](../terrain/chunk_state_machine.md) and preparation/release call sites when
adapting it: bypassing state transitions is intentional, bypassing required data is not.

`support.cpp` replaces the world-seed getter and the rendering-dependent block loader. Its
JSON loading must agree with the current production loader for every property consulted by
generation, masks, structures, or decorators. Missing cube/type, surface-mounted state, or
emitter metadata can silently change placement. Use real biome and structure initialization.
Reinitialize seed-dependent noise graphs whenever changing the seed.

The archived builder imports the setup prefix of an earlier `terrain_noise_perf/build_bench.py`;
that file is included in the reference too, so the old ignored directories are not the only
copy. Fold the relevant setup into the new disposable script. Do not recreate the chain of
old experiment dependencies. Its absolute MSVC/SDK paths, project XML selection, object/library
lists, source-extraction markers, and compatibility macros are historical assumptions.

## Build comparable variants

Capture the baseline revision and the relevant sources/headers before editing. Give each
variant a separate generated-source/include directory and executable. Header lookup must
select that variant's Chunk layout; reusing a current header with an older implementation
can invalidate the comparison even when it compiles. Rebuild any support object affected by
a changed header or implementation. Never include the main application's Chunk object as
well as the extracted method definitions.

Use the same optimized compiler, flags, assertions, and FastNoise2/FastSIMD configuration for
both variants. Discover these from the current configured build instead of copying an SDK
version or choosing arbitrary libraries from an old build tree. The historical run used
MSVC 19.44, `/O2 /Ob1 /MD`, assertions, and the optimized noise libraries; these values explain
that result rather than prescribe future toolchains. See [build configurations](../build/configs.md),
including its Windows duplicate-environment recovery only if the normal build fails.

Inspect generated instrumented code. The reference's string replacement and brace counting
depend on old source text and are not a C++ parser; assert each intended insertion occurs
exactly once. Avoid timers inside voxel/noise-sample loops. Coarse stage timers should impose
the same overhead on baseline and candidate. Preserve compiler optimization of the real code.

## Choose a workload and endpoint

Use fixed seeds and a fixed coordinate manifest covering varied terrain heights and biomes,
including negative coordinates and chunks around the origin. The example finds a 2x2 patch
per surface biome for four seeds, plus four origin-border chunks: 208 unique targets with the
then-current twelve biomes. Freeze the manifest across variants; if changing biome selection,
do not rediscover different coordinates for each executable. Verify discovery actually covers
the desired biomes. Expand cave coverage as more cave biomes and decorators are added.

For generation alone, stop after block filling and structure candidate creation. If work
moves between generation and decoration, measure through decoration as well: deferred work
is only a saving if the broader total falls. Report stages alongside the total so movement
between stages and drift in unchanged code are visible.

For structures/decorators, generate actual neighbors, immutable terrain masks, and candidate
lists first. The reference wires each target's four cardinal neighbors and a 3x3 structure
neighborhood, requiring 832 chunks including halos for its 208 targets. Re-evaluate that
radius and ordering against current structure bounds. Halo generation supplies prerequisites;
its time is excluded from the reported target average. Keep masks immutable after structures
start. Reading or rebuilding masks from already decorated blocks changes support semantics.

Reset blocks, candidates, and block states before every sweep. Clear scratch memory at the
same boundary on both variants, and document whether its reset is timed. Reproduce temporary
metadata allocation and release, including halos. `vector.clear()` keeps capacity, so it
cannot stand in for production storage release when allocator cost is part of the question.
The reference retains persistent chunk/vector capacity after warmup but releases deferred
cave metadata each sweep. Label this as a warm CPU kernel experiment; use fresh chunk objects
if cold allocation is what needs measuring. Its preparation also clears reused block arrays,
so this stage is not a direct measurement of first-use application allocation.

## Separate validation from timing

Run correctness snapshots in separate process invocations. Reconstructing cave IDs, hashing
whole chunks, or writing large snapshots between two timers still perturbs cache state and
CPU behavior even when that work is outside the timed intervals. Verification-only hooks
belong in separate executables. A small consumed checksum can keep outputs observable during
timing, but checksum equality alone is not the correctness test.

For a behavior-preserving optimization, compare complete pre-structure and final block
arrays, surface heights, structure candidates, and sorted block states, including decoration
orientation. Compare chunk manifests too. When changing deferred cave data, reconstruct every
original cave-air biome from the retained fields and biases before releasing them, including
unsupported cave interiors. Matching final blocks alone would miss mistakes there.

If a shape change is intentional, replace exact baseline equality for the affected outputs
with explicit changed-voxel/height statistics and representative visual checks. Still require
repeatability and agreement for overlapping world samples. Never excuse seam, bounds, or
uninitialized-data failures as an acceptable shape change. Keep cave temperature and humidity
independent: future biomes can use different combinations even if current biomes look redundant.

The reference contains two useful families of independent checks:

- Noise-grid checks compare a larger request against overlapping chunks with negative
  origins and differently clipped Y bands, plus very short bands and lattice endpoints.
  World coordinates must agree regardless of request shape; see
  [noise sampling](../terrain/chunk_generator.md).
- Decorator fixtures compare bit-filter candidates and their order to scalar six-face
  support queries. Check individual directional bits too. Cover chunk edges, 64-bit word
  carries, support above the cave ceiling, multiple faces, non-cube supports, original cave
  air occupied by structures, empty surface decorators, and supports above the terrain top.
  Preserve real generator invariants such as bedrock at Y=0. Check metadata capacity release,
  not just vector size. The archived fixture dimensions/heights must adapt to current constants.

## Run and interpret the comparison

Build and validate all variants before measurement. Run one executable at a time without
concurrent compiles or other benchmark processes. Use a warmup sweep and several measured
sweeps, then alternate baseline/candidate order across short seed-specific processes. The
example uses four seeds, five measured sweeps, and three rounds: 3,120 observations per
variant, consisting of fifteen repetitions of 208 unique targets. Do not present these as
3,120 independently selected worlds.

Record per-chunk stage timings with seed, coordinates, iteration, and variant. Compare means
over identical workloads, per-biome results, and matched seed/round deltas. Short alternating
processes reduce order/thermal drift; they do not control CPU frequency or background load.
If unchanged stages move broadly, investigate that noise before claiming a small improvement.

Report the baseline commit, candidate diff, hardware/build settings, unique and repeated
sample counts, included stages, allocation policy, output checks, and paired variation.
Calculate time reduction as `100 * (baseline - candidate) / baseline`; distinguish it from
throughput increase. These measurements omit scheduling, meshing, rendering, and application
startup and do not establish FPS or whole-world load-time improvements.

For context, baseline `7c6240a` versus retained changes in `21e7056` gave 2.289 to 2.149 ms
through decoration (6.1% less) on an i7-10750H. Decoration fell from 0.244 to 0.106 ms; all
sampled outputs and 48 synthetic fixtures matched. This is a historical result, not a
performance threshold. Earlier noise experiments stopped at generation, so their absolute
totals cannot be compared directly with this endpoint.

Build the real application after the change. Keep raw CSVs, snapshots, and a report in the
ignored experiment directory while investigating. Preserve transferable reasoning here;
do not commit executables, object files, noise grids, or temporary production timers. The
reference snapshot is enough to reconstruct the approach if old build artifacts are gone.
