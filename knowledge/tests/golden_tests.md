_Last edited: 2026-09-01_

# Golden Image Tests

`BiomeinatorTests.exe` (`src/tests/main.cpp`) reads `tests/tests.json`, launches
`Biomeinator.exe` once per entry with the entry's args plus `--testOutput=<path>` (camera locked,
GUI hidden, animation paused), and compares the screenshot against the entry's golden with
RMSE over 8-bit RGB normalised to [0, 1]. `-f <regex>` filters by test name. Every run writes
`<name>_GENERATED.png`, `<name>_GOLDEN.png` and `<name>_DIFF.png` to `build/test_output/`,
which is the place to look when a test fails.

## Three kinds of golden image

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
- **`golden_diffuseAlbedo.png`** — an engine golden like `golden.png`, but of the
  `--debugView="diffuseAlbedo"` output (the DLSS-RR guide buffer) rather than the beauty
  image. Used by the `diffuse_albedo_modulation*` and `water_reflection_diffuse_albedo`
  entries with near-zero thresholds, since the albedo view has no path-tracing noise.

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
