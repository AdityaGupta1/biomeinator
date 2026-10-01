_Last edited: 2026-09-30_

# Rendering Tests

`BiomeinatorRenderingTests.exe` (`src/tests/main.cpp`) reads `tests/tests.json`, launches
`Biomeinator.exe` once per entry with the entry's args plus `--renderingTestOutput=<path>`
(camera locked, GUI hidden, animation paused), and compares the screenshot against its reference image with
RMSE over 8-bit RGB normalised to [0, 1]. `-f <regex>` filters by test name, while
`--test <name>` selects exactly one entry and fails rather than silently succeeding if the name
does not exist. Every run writes
`<name>_GENERATED.png`, `<name>_GOLDEN.png` and `<name>_DIFF.png` to `build/test_output/`,
which is the place to look when a test fails.

Run the test runner, or CTest when selecting rendering tests, **outside the agent sandbox** so
the child renderer inherits normal permissions. On Windows, the sandbox denies NVIDIA
telemetry's named-pipe open with `ERROR_ACCESS_DENIED`, leaving `slShutdown()` waiting for
telemetry cleanup after the screenshot is saved. Launching the same executable outside the
sandbox allows normal shutdown; changing its process environment does not remove the sandbox
restriction. The CTest path was verified this way on 2026-09-30 with
`rendering::diffuse_albedo_modulation`, which passed and exited in 6.23 seconds.

## CTest registration

CMake reads `tests/tests.json` at configure time and registers every manifest entry as a separate
`rendering::<name>` CTest test with the `rendering` label. The manifest is a configure dependency, so
adding, removing, or renaming an entry regenerates the test list. Each entry invokes the runner's
exact-name mode; duplicate names instead fail CMake configuration.

All rendering test entries share the `biomeinator_gpu` CTest resource lock. This keeps renderer
processes from competing for the GPU under parallel CTest runs without preventing CPU-only tests from being
scheduled concurrently. Each invocation removes only its own generated, copied-golden, and diff
images, so diagnostics from other entries survive individual or `--rerun-failed` runs.

`BiomeinatorRenderingTests` is an executable build target: building it also builds `Biomeinator`,
but does not execute the rendering tests. Launching `BiomeinatorRenderingTests.exe` without
arguments runs every entry in the manifest; `--test <name>` and `-f <regex>` select a subset.
Likewise, building `BiomeinatorUnitTests` does not run its suite; launching its executable without
arguments runs all unit cases. CTest provides individual results and label selection for both.

Build both executables, then run both suites or select one label:

```powershell
cmake --build build --config RelWithDebInfo --target BiomeinatorUnitTests BiomeinatorRenderingTests
ctest --test-dir build -C RelWithDebInfo --output-on-failure
ctest --test-dir build -C RelWithDebInfo -L unit --output-on-failure
ctest --test-dir build -C RelWithDebInfo -L rendering --output-on-failure
```

## Procedural entries

A test entry has a `scene` (glTF), a `world` (saved export), or neither: a procedurally generated
voxel world configured entirely by its args (`--voxelMode`, `--worldSeed`, `--renderDistance`,
`--cameraX/Y/Z`, `--cameraYaw/Pitch` in degrees, as a world export stores its camera). Headless
capture waits for the complete geometry ring before accumulating. Use procedural entries for
features that should track current generation, like `grass_biome_blend`'s real biome edge: an
imported world keeps its saved blocks but still takes grass tint from the current biome noise,
so it would test neither the old nor the new biomes cleanly. Procedural goldens need
regenerating whenever world generation changes; keep their render distance small to bound
generation time.

## Reference images

Each glTF test folder holds a `.blend` (the source of truth), the exported `.gltf`/`.bin`
(see [scene → blender_export.md](../scene/blender_export.md)), and up to three PNGs that mean
different things:

- **`golden.png`** — the engine's *own* output, i.e. a regression golden. It is produced by
  running the test and copying `build/test_output/<name>_GENERATED.png` over it, never by
  rendering in Blender. Regenerate it only when a change to the output is intended; the
  `<name>` entry in `tests.json` uses a tight threshold because it is only absorbing
  accumulation noise, not model differences.
- **`golden_blender_no_tonemap.png`** — a Cycles render of the same `.blend`, saved through the
  Raw view transform. The `<name>_blender_no_tonemap` entry renders with `--tonemapping=0`
  against it (usually also `--refractionIndirectPassthrough=false`, since passthrough is an
  engine approximation Cycles has no equivalent of). This is the physical-correctness check;
  its threshold is looser because the error includes both renderers' noise and genuine model
  differences.
- **`golden_blender.png`** — the same Cycles render saved through the scene's tonemapped view
  transform (Khronos PBR Neutral). Nothing in `tests.json` references it; it exists only for
  eyeballing against `golden.png`. New tests do not need one.
- **`golden_diffuseAlbedo.png`** / **`golden_specularAlbedo.png`** — engine goldens like
  `golden.png`, but of the `--debugView="diffuseAlbedo"` / `"specularAlbedo"` output (the
  DLSS-RR guide buffers) rather than the beauty image. Used by the
  `diffuse_albedo_modulation*`, `water_reflection_diffuse_albedo` and `crystal_caves_*_albedo`
  entries with near-zero thresholds, since the guide views carry no accumulation noise. Run
  them with `--antialiasingMode=0 --noJitter`. The specular view is bit-exact run to run, but
  the diffuse view is not, which is why the voxel entries sit at 0.001 rather than the 0.0001
  the glTF ones hold: a diffuse first bounce takes its guide from `bsdf * cos / pdf`, which is
  `albedo` algebraically but not to the last bit, and the residual depends on the sampled
  direction — whose RNG is seeded from the frame number, which a voxel world reaches after a
  variable number of chunk-streaming frames. It moves only pixels already sitting on an 8-bit
  quantization boundary, by one LSB (~0.0002 RMSE over a 480x270 cave).

A test for a feature the engine does not support yet still gets a `golden.png` snapshot of
the current (wrong) output, so the `<name>` test goes red the moment the feature lands and is
regolded then; the `_blender_no_tonemap` test carries the actual target.

## Producing Blender goldens

Render headlessly with the scene's own sample count and bounces, on the OptiX device, then
save the one render twice: `save_render` with the scene's view transform for
`golden_blender.png`, switch `view_settings.view_transform` to `'Raw'`, and `save_render`
again for `golden_blender_no_tonemap.png`. The `.blend` must be saved with the tonemapped view
transform, not Raw. Scenes with transmissive materials need Cycles' Open Shading Language
option on, because the node group's dielectric is an OSL closure.

Scenes with *rough* transmission additionally set `cycles.emission_sampling = 'NONE'` on every
emitter material and 64 bounces in the `.blend` (`glass_different_roughness` is set up this way,
with 32768 samples). Cycles' `bsdf_microfacet_eval` for the glass closure credits refraction
directions its sampler can never produce, and eval is only used by light sampling, so turning
light sampling off makes Cycles' rough glass consistent with its own sampler (and with the
engine, see [shaders → materials.md](../shaders/materials.md)); the high bounce count removes
the two renderers' different bounce-limit accounting on long silhouette paths. The matching
`_blender_no_tonemap` entry runs with `--maxPathDepth=64` and 16384 frames, and its threshold
(0.02) is set by the combined noise floor (~0.016), not by model differences.
