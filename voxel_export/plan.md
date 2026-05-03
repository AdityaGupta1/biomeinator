# World Export/Import — Plan

## Context

Serialize entire terrain state to disk for voxel mode regression tests. Export captures all chunks (including boundary), camera state, and settings. Import loads them back and lets the normal terrain update loop build geometry/BLASes. Eventually reusable for chunk offloading.

---

## Binary Region Format (`region_X_Z.bin`)

```
File header (16 bytes):
  uint32_t magic        = 0x42494F4D ("BIOM")
  uint16_t version      = 3
  int32_t  regionX
  int32_t  regionZ
  uint16_t numPopulatedChunks

Per populated chunk (repeated numPopulatedChunks times):
  uint16_t chunkLocalIdx            (0..1023)
  uint8_t  importLevel              (raw `ChunkState` enum value: HAS_TERRAIN=2, HAS_ALL_BLOCKS=6)
  uint32_t compressedBlocksSize     (always > 0)
  uint32_t compressedStructuresSize (0 if no structures)
  [compressedBlocksSize bytes]      LZ4-compressed block+biome payload
  [compressedStructuresSize bytes]  LZ4-compressed structures payload

Block+biome payload (uncompressed = 262400 bytes):
  uint16_t blocks[131072]
  uint8_t  biomes[256]

Structures payload (uncompressed, variable size):
  uint32_t numStructures
  per structure (13 bytes, tightly packed):
    uint8_t  type
    int32_t  pos_WS[3]
```

Segments are NOT serialized — they are deterministic from blocks and regenerated on import via the normal `generateSegments` task.

## `scene.json`

```json
{
  "version": 1,
  "camera": {
    "posInt": [0, 196, 0],
    "posFloat": [0.0, 0.5, 0.0],
    "phi": 0.0,
    "theta": 3.14159
  },
  "renderDistance": 8,
  "worldSeed": 1738,
  "regions": [[0, 0], [-1, 0]]
}
```

## Import Methodology — checkpoint-based early-return

### Core idea

Imported chunks load their `blocks`, `biomes`, and `structures` vectors from disk, then start at `state = NEEDS_TERRAIN` like any fresh chunk. They traverse the **full** state machine: `NEEDS_TERRAIN` → `GENERATING_TERRAIN` → `HAS_TERRAIN` → `AWAITING_STRUCTURE_NEIGHBORS` → `NEEDS_FILL_STRUCTURES` → `FILLING_STRUCTURES` → `HAS_ALL_BLOCKS` → `NEEDS_SEGMENTS` → `GENERATING_SEGMENTS` → `NEEDS_GEOMETRY` → `GENERATING_GEOMETRY` → `HAS_GEOMETRY`.

Each terrain task **early-returns its inner data work** when the imported checkpoint already covers it. The state advance + neighbor counter side effects (`numReadyStructureNeighbors`, `numNeighborsWithBlocks`) **always** run — these distributed counters drive the state machine and must fire identically to fresh generation.

**Early-return is a correctness requirement, not an optimization.** Re-running inner work would re-stamp blocks that are already final, and even if individual operations are idempotent on paper, we do not want any chance of divergence between imported and freshly-generated state.

### Per-task behavior

`Chunk` gains a field `ChunkState importLevel` (default `NEEDS_TERRAIN`, set by `loadSerializedData` to either `HAS_TERRAIN` or `HAS_ALL_BLOCKS`).

| Task | If `importLevel >= HAS_TERRAIN` | If `importLevel >= HAS_ALL_BLOCKS` |
|---|---|---|
| `generateTerrain` (`task_generateTerrain`) | Skip `fillTerrainBlocksAndCreateStructures`. Only call `advanceState(HAS_TERRAIN)` + `setDirty()`. | Same — already covered by `>= HAS_TERRAIN`. |
| `checkStructureNeighbors` (`task_checkStructureNeighbors`) | Always runs unchanged. Cheap pointer walk; the 5×5 counter increments on neighbors are required. | Same. |
| `fillStructuresAndDecorators` (`task_fillStructures`) | Run inner work normally — imported blocks at `HAS_TERRAIN` lack neighbor-structure overlap and decorators. | Skip both the `fillStructureBlocks` loop over `structureNeighbors` AND the decorator pass. Still call `advanceState(HAS_ALL_BLOCKS)` and run the `numNeighborsWithBlocks.fetch_add` loop on the 4 cardinal neighbors. |
| `generateSegments` (`task_generateSegments`) | Always runs. Segments are not serialized; they are deterministic from blocks. | Same. |
| `task_generateGeometry` | Always runs (geometry is rebuilt from blocks + segments). | Same. |

### Why every counter side effect must still run

- `checkStructureNeighbors` increments `numReadyStructureNeighbors` on each of the 25 neighbors in the chunk's 5×5 footprint. A neighbor only advances `HAS_TERRAIN` → `NEEDS_FILL_STRUCTURES` once its counter hits 25 *and* its state is `>= HAS_TERRAIN`. Skipping this for imported chunks would leave neighbors stuck.
- `fillStructuresAndDecorators` increments `numNeighborsWithBlocks` on each of the 4 cardinal neighbors. A neighbor only advances `HAS_ALL_BLOCKS` → `NEEDS_SEGMENTS` once its counter hits 4. Skipping this for imported chunks would leave neighbors stuck.

These counters are also untouched by `setNeighbors` (`chunk.cpp:69-70`) — the only place they advance is the task body. So early-return must wrap only the data-mutating section, not the counter section.

### Why imported `HAS_TERRAIN` is safe

A chunk at `HAS_TERRAIN` has terrain blocks + biomes + own-chunk `structures` populated, but **NOT** decorator blocks and **NOT** neighbor-structure overlap blocks. Re-running the fillStructures inner work on import-loaded `HAS_TERRAIN` blocks produces the same final state as fresh generation, because:
- `fillStructureBlocks` reads `structures` lists from neighbors (which are also imported or freshly generated, both deterministic).
- Decorators use a `worldSeed`-seeded RNG and reads/writes deterministically from terrain blocks.

### Import sequence

1. Validate `--world` path exists, contains `scene.json`. Fatal exit if missing/malformed.
2. Parse `scene.json`. Apply `renderDistance` and `worldSeed` to `SettingsManager` **before any terrain code runs** (decorators depend on `worldSeed`).
3. For each region in `scene.json`:
   - Open `region_X_Z.bin`. Validate magic + version. Fatal on mismatch.
   - Insert `Region` into `regions` map. Region neighbor pointers are wired up by the normal update loop.
   - For each chunk entry: LZ4-decompress block+biome payload (always present) and structures payload (if `compressedStructuresSize > 0`). Call `chunk->loadSerializedData(blocks, biomes, structures, importLevel)`. The chunk's `state` stays at `NEEDS_TERRAIN`; `importLevel` is set to the value from disk.
4. Count chunks where `importLevel == HAS_ALL_BLOCKS` and the chunk position falls within `createBlasDistance` of the imported camera position → set `expectedBlasBuildChunks`. Set `worldImportActive = true`.
5. Restore camera via `Renderer::restoreCamera(...)`.
6. Call `Terrain::setDirty()`.

The normal update loop then:
- Wires region + chunk neighbor pointers via `try_emplace` + `setNeighbors(true)`.
- Creates outer-ring stub chunks at `NEEDS_TERRAIN` (for pointer linkage); these are not imported, just spawned as needed.
- Dispatches each task in order. Imported chunks early-return inner work and propagate state + counters as described above.

---

## Steps

### Step 1: Accessor scaffolding

Add getters/setters needed by serialization code. No behavioral changes — purely additive.

**camera.h/cpp:** `getPhi()`, `getTheta()`, `restoreState(glm::ivec3 posInt, glm::vec3 posFloat, float phi, float theta)`
**chunk.h/cpp:** `loadSerializedData(std::vector<Block>&& blocks, std::vector<Biome>&& biomes, std::vector<glm::uvec3>&& segments, ChunkState state)` — takes ownership of deserialized data, sets state. Chunk handles its own internals.
**renderer.h/cpp:** `restoreCamera(glm::ivec3 posInt, glm::vec3 posFloat, float phi, float theta)` — thin wrapper calling `renderState.camera.restoreState()`

**Verify:** Builds with no errors. No behavior change.

### Step 2: Settings and keybind

**settings_manager.cpp:** Register `--world` option (string, default ""). After COPY_SETTING block, if world is non-empty, force `voxelMode = true`.
**window_manager.cpp:** Ctrl+E in `onKeyDown()` calls `Terrain::exportWorld()` (stub for now).
**terrain.h:** Declare `exportWorld()`, `importWorld()`, `isWorldFullyLoaded()`.
**terrain.cpp:** Add empty stubs + BLAS tracking statics.

**Verify:** Builds. `--world=foo` forces voxelMode on. Ctrl+E in voxel mode calls stub (log message).

### Step 3: Export

Implement `Terrain::exportWorld()` in terrain.cpp.

1. Build output dir: `~/Documents/biomeinator/exports/YYYY.MM.DD_HH-MM-SS/` (reuse SHGetFolderPathW + SYSTEMTIME pattern from renderer_screenshot.cpp)
2. Read camera state via `Renderer::getCamera()` (phi, theta, posInt, posFloat)
3. Read renderDistance, worldSeed from SettingsManager
4. Iterate `regions` map. For each region containing any chunk with `state >= HAS_TERRAIN`:
   - Write region binary file per format above
   - Per chunk: gate `state >= HAS_TERRAIN`. Compute `importLevel`:
     - `state >= HAS_ALL_BLOCKS` → `importLevel = HAS_ALL_BLOCKS`
     - else → `importLevel = HAS_TERRAIN`
   - LZ4-compress blocks+biomes (always). LZ4-compress structures (if non-empty).
5. Write `scene.json` using nlohmann::json

Outer-ring stub chunks (created by `setNeighbors(true)` for pointer linkage, state == `NEEDS_TERRAIN`) are skipped by the gate. They will be re-created on import the same way during the normal update loop.

**Verify:** Run voxel mode, fly somewhere, Ctrl+U. Check exports folder exists with scene.json + .bin files. Validate JSON. Hex-check .bin files for BIOM magic + version 3. Log compressed vs uncompressed sizes.

### Step 4: Import — chunk hooks + importWorld()

Two parts:

**4a. Chunk-side: add `importLevel` field and early-return hooks.**

- `chunk.h`: add `ChunkState importLevel{ ChunkState::NEEDS_TERRAIN };` member.
- `chunk.h/cpp`: change `loadSerializedData` signature to take `(std::vector<Block>&&, std::vector<Biome>&&, std::vector<Structure>&&, ChunkState importLevel)`. (Drop the `state` param + the segments vector.) The function moves data in, sets `this->importLevel = importLevel`, leaves `this->state == NEEDS_TERRAIN`.
- `Chunk::generateTerrain`: if `importLevel >= HAS_TERRAIN`, skip the body. Always call `advanceState(HAS_TERRAIN)` and `Terrain::setDirty()`. The `blocks.resize(numChunkBlocks)` / `biomes.resize(chunkSizeXZSquare)` calls become no-ops if vectors are already sized — keep them or guard, either is fine.
- `Chunk::fillStructuresAndDecorators`: if `importLevel >= HAS_ALL_BLOCKS`, skip the entire `for (Chunk* structureNeighbor : this->structureNeighbors)` block AND the decorator double-loop. Always call `advanceState(HAS_ALL_BLOCKS)` and run the existing `for (Chunk* neighborChunk : this->neighbors)` `numNeighborsWithBlocks.fetch_add` loop.
- `Chunk::checkStructureNeighbors`: unchanged. Must always run regardless of import level (drives 5×5 counter).
- `Chunk::generateSegments`: unchanged. Segments are not exported — always regenerate from blocks.

**4b. `Terrain::importWorld()` in terrain.cpp.**

1. Validate `--world` path exists and contains `scene.json`. Fatal `exit(1)` if missing/malformed.
2. Parse `scene.json`.
3. Apply `renderDistance` and `worldSeed` to `SettingsManager` before any terrain task can run (decorators read `worldSeed`).
4. For each region coord pair in `scene.json`:
   - Derive filename `region_X_Z.bin`. Fatal if missing.
   - Read header, validate magic (`0x42494F4D`) + version (`3`). Fatal on mismatch.
   - `regions.emplace(regionPos, std::make_unique<Region>(regionPos))`.
   - For each chunk entry:
     - Read `localIdx`, `importLevel`, `compressedBlocksSize`, `compressedStructuresSize`.
     - LZ4-decompress block+biome payload (262400 bytes uncompressed). Split into `std::vector<Block> blocks(numChunkBlocks)` and `std::vector<Biome> biomes(chunkSizeXZSquare)`.
     - If `compressedStructuresSize > 0`: LZ4-decompress structures. Read `numStructures` then unpack each `(uint8_t type, int32_t pos[3])` into `std::vector<Structure>`.
     - `Chunk* chunk = region->createChunk(localChunkPos);`
     - `chunk->loadSerializedData(std::move(blocks), std::move(biomes), std::move(structures), importLevel);`
5. Count chunks where `importLevel == HAS_ALL_BLOCKS` AND `chebyshevDistance(chunkPos, cameraChunkPos) <= createBlasDistance` → set `expectedBlasBuildChunks`. Set `worldImportActive = true`.
6. Restore camera via `Renderer::restoreCamera(posInt, posFloat, phi, theta)`.
7. Call `Terrain::setDirty()`.

The normal update loop takes over from here — sets region/chunk neighbor pointers via `setNeighbors(true)` (which spawns outer-ring `NEEDS_TERRAIN` stubs as needed), then dispatches all terrain tasks. Imported chunks traverse the full state machine; their early-return hooks skip already-baked data.

**Verify:** Export a scene, then `--world=<path> --lockCamera=true`. Terrain renders. No missing faces at chunk borders. No asserts. State counters resolve cleanly (no stuck chunks).

### Step 5: Renderer init wiring + BLAS tracking

Wire `Terrain::importWorld()` into renderer init (after `Terrain::init`, before first frame).
Modify `addChunkToCreateBlas()` to increment `completedBlasBuildChunks` when `worldImportActive`.
Implement `Terrain::isWorldFullyLoaded()` returning `!worldImportActive || completedBlasBuildChunks >= expectedBlasBuildChunks`.

The BLAS-tracking counter only counts chunks that were imported as `HAS_ALL_BLOCKS` within `createBlasDistance`. Imported `HAS_TERRAIN` chunks at the boundary do not get BLASes (they're outside `createBlasDistance` by construction, since `HAS_ALL_BLOCKS`-imported chunks are the ones inside the active BLAS ring at export time).

**Verify:** Log when `isWorldFullyLoaded()` transitions true. Should fire after all imported `HAS_ALL_BLOCKS` chunks within `createBlasDistance` have completed BLAS builds.

### Step 6: Test loader integration

**test_loader.cpp:** Support optional `world` field in test JSON. If present, append `--world=<path>` instead of `--scene=<path>`.

**Verify:** Add test entry with `world` field to tests.json. Run test harness, confirm it passes args correctly and golden comparison works.

---

## Future optimization (not for initial impl)

Structure entries currently serialize as `uint8_t type` + `int32_t pos_WS[3]` (13 bytes pre-LZ4). Can compress to 4 bytes by storing chunk-local position instead of world position:
- 8 bits: type
- 4 bits + 9 bits + 4 bits: local x (0..15) + local y (0..511) + local z (0..15) — 17 bits, padded to 24
- Total 25 bits used, fits in 4 bytes (`uint32_t`).

Origin chunk is implicit from where the structure entry is stored. Skip for now — keep format simple until profiler shows it matters.
