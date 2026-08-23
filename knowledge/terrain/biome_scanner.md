_Last edited: 2026-08-23_

# BiomeScanner

`BiomeScanner` is a separate CMake target: a local HTTP server serving an interactive seed map
plus JSON/binary APIs for biome grids and seed search.

## Why the biome noise lives in its own module

The surface biome field (temperature/humidity/peak/inland nodes, flood factor, swamp override,
`fillBiomeRect`) was extracted from `chunk_generator.cpp` into `src/terrain/biome_noise.{h,cpp}`
(`BiomeNoiseFields` namespace) so the scanner evaluates *exactly* the generation's biome logic by
compiling the same translation unit — no reimplementation to drift. The scanner links only the
biome/noise translation units (`BIOME_SCANNER_SRC` in CMakeLists) — no engine, no D3D12 — so it
starts instantly and runs headless.

`BiomeNoiseFields::init(seed)` also derives the world-wide `noiseOffsetXZ` (it must: the scanner
has no ChunkGenerator), and `ChunkGenerator::init` reads it back via `getNoiseOffsetXZ()` for the
terrain/cave/swamp noises.

## Gotchas

- The map shows the macro biome field (`fillBiomeRect` skips per-column jitter), so biome borders
  in-game fuzz a few blocks past what the map shows.
