_Last edited: 2026-08-15_

# BiomeScanner

`BiomeScanner` is a separate CMake target: a local HTTP server (cpp-httplib, port 8080 or argv[1])
serving an interactive seed map at `/` plus JSON/binary APIs for biome grids and seed search. Run
it and open `http://127.0.0.1:8080`.

## Why the biome noise lives in its own module

The surface biome field (temperature/humidity/peak/inland nodes, flood factor, swamp override,
`fillBiomeRect`) was extracted from `chunk_generator.cpp` into `src/terrain/biome_noise.{h,cpp}`
(`BiomeNoiseField` namespace) so the scanner evaluates *exactly* the generation's biome logic by
compiling the same translation unit — no reimplementation to drift. The scanner links only
`biome_noise.cpp`, `biome.cpp`, `decorator.cpp`, `structure_gen.cpp`, and `logger.cpp` — no
engine, no D3D12, so it starts instantly and runs headless.

`BiomeNoiseField::init(seed)` also derives the world-wide `noiseOffsetXZ` (it must: the scanner
has no ChunkGenerator), and `ChunkGenerator::init` reads it back via `getNoiseOffsetXZ()` for the
terrain/cave/swamp noises. `StructureGen`'s member functions live in `structure_gen.cpp`
(not `structure.cpp`) purely so linking the biome table doesn't pull in structure placement and
chunk code.

## Gotchas

- The map shows the macro biome field (`fillBiomeRect` skips per-column jitter), so biome borders
  in-game fuzz a few blocks past what the map shows.
- The server serves `src/scanner/index.html` from the source tree per request — edit and refresh,
  no rebuild.
- `BiomeNoiseField` state is global; the scanner serializes all seed switches and fills behind one
  mutex. A `/api/search` leaves the server's noise state on the last scanned seed, so the page
  refetches the map after a search finishes.
- Map colors are a scanner-local palette in `scanner/main.cpp` (chunkbase-style, sized against
  `Biome::COUNT`) — in-game grass tints are all similar greens and unreadable as a map.
