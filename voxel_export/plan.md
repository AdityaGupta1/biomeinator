# World Export/Import — Plan

## Context

Serialize entire terrain state to disk for voxel mode regression tests. Export captures all chunks (including boundary), camera state, and settings. Import loads them back and lets the normal terrain update loop build geometry/BLASes. Eventually reusable for chunk offloading.

---

## Binary Region Format (`region_X_Z.bin`)

```
File header (16 bytes):
  uint32_t magic        = 0x42494F4D ("BIOM")
  uint16_t version      = 1
  int32_t  regionX
  int32_t  regionZ
  uint16_t numPopulatedChunks

Per populated chunk (repeated numPopulatedChunks times):
  uint16_t chunkLocalIdx          (0..1023)
  uint8_t  chunkState             (ChunkState enum value)
  uint8_t  flags                  (bit 0 = hasBlocks, bit 1 = hasSegments)
  uint32_t compressedBlocksSize   (0 if !hasBlocks)
  uint32_t compressedSegmentsSize (0 if !hasSegments)
  [compressedBlocksSize bytes]    LZ4-compressed block+biome payload
  [compressedSegmentsSize bytes]  LZ4-compressed segments payload

Block+biome payload (uncompressed = 262400 bytes):
  uint16_t blocks[131072]
  uint8_t  biomes[256]

Segments payload (uncompressed, variable size):
  uint32_t numSegments
  uint32_t segmentData[numSegments * 3]  (uvec3 per segment, tightly packed)
```

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

## Import State Handling

The update loop's `Terrain::update()` handles ALL neighbor linking automatically:
- Iterates regions, sets region neighbors via `try_emplace` + `setNeighbor`
- Iterates chunks, calls `setNeighbors(true)` which finds imported chunks via `getChunk()`
- For edge-of-export chunks, `setNeighbors(true)` may create new empty chunks — harmless since those are outside `createBlasDistance`

`setNeighbors` does NOT touch `numNeighborsWithBlocks` (chunk.cpp line 69-70). So imported chunks at `HAS_ALL_BLOCKS` will never advance to `NEEDS_SEGMENTS` via the normal gate — `HAS_ALL_BLOCKS` is effectively a dead/inert state for imported chunks. This is exactly what we want for boundary chunks that just provide block data.

Import state table:

| Has blocks | Has segments | Import as | Behavior |
|---|---|---|---|
| no | no | skip | Nothing to store |
| yes | no | HAS_ALL_BLOCKS | Inert — provides blocks for neighbor face culling |
| yes | yes | NEEDS_GEOMETRY | Active — update loop builds geometry + BLAS |

No manual neighbor setup. No second pass. No `numNeighborsWithBlocks` manipulation.

Import must apply `renderDistance` and `worldSeed` from scene.json so update loop distance bounds match export.

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
4. Iterate `regions` map. For each region with populated chunks:
   - Write region binary file per format above
   - Per chunk: check state, LZ4-compress blocks+biomes if hasBlocks, LZ4-compress segments if hasSegments
5. Write `scene.json` using nlohmann::json

**Verify:** Run voxel mode, fly somewhere, Ctrl+E. Check exports folder exists with scene.json + .bin files. Validate JSON. Hex-check .bin files for BIOM magic. Log compressed vs uncompressed sizes.

### Step 4: Import

Implement `Terrain::importWorld()` in terrain.cpp.

1. Validate `--world` path exists and contains scene.json. If missing or malformed, log error and `exit(1)`.
2. Parse scene.json
3. Apply renderDistance and worldSeed from JSON to SettingsManager
4. For each region coord pair in scene.json, derive filename (`region_X_Z.bin`), verify file exists. Fatal error if any missing.
   - Read header, validate magic + version. Fatal error on mismatch.
   - Create `Region` at stored position, insert into `regions` map
   - For each chunk entry:
     - Decompress LZ4 blocks+biomes and segments (if present)
     - Compute import state: NEEDS_GEOMETRY if hasSegments, else HAS_ALL_BLOCKS
     - Call `chunk->loadSerializedData(blocks, biomes, segments, importState)`
5. Count chunks that will need BLAS (hasSegments = true within createBlasDistance) → set `expectedBlasBuildChunks`, set `worldImportActive = true`
6. Restore camera via `Renderer::restoreCamera(posInt, posFloat, phi, theta)`
7. Call `Terrain::setDirty()`

Normal update loop takes over from here — sets region neighbors, chunk neighbors, advances NEEDS_GEOMETRY chunks through geometry gen + BLAS building. HAS_ALL_BLOCKS chunks sit inert.

**Verify:** Export a scene, then `--world=<path> --lockCamera=true`. Terrain renders. No missing faces at chunk borders. No asserts.

### Step 5: Renderer init wiring + BLAS tracking

Wire import into renderer init (after Terrain::init, before first frame).
Modify `addChunkToCreateBlas()` to increment completion counter when worldImportActive.
Implement `isWorldFullyLoaded()`.

**Verify:** Log when isWorldFullyLoaded() transitions true. Should fire after all chunks have BLASes.

### Step 6: Test loader integration

**test_loader.cpp:** Support optional `world` field in test JSON. If present, append `--world=<path>` instead of `--scene=<path>`.

**Verify:** Add test entry with `world` field to tests.json. Run test harness, confirm it passes args correctly and golden comparison works.
