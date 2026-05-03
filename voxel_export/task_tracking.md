# World Export/Import — Task Tracking

## Step 1: Accessor scaffolding ✅
- [x] camera.h: add getPhi(), getTheta(), restoreState() declarations
- [x] camera.cpp: implement restoreState()
- [x] chunk.h: add loadSerializedData() declaration
- [x] chunk.cpp: implement loadSerializedData()
- [x] renderer.h: add restoreCamera() declaration
- [x] renderer.cpp: implement restoreCamera() — wraps camera.restoreState()
- [x] Verify: build succeeds

## Step 2: Settings and keybind ✅
- [x] settings_manager.cpp: register "world" option + voxelMode override
- [x] terrain.h: declare exportWorld(), importWorld(), isWorldFullyLoaded()
- [x] terrain.cpp: add empty stubs + BLAS tracking statics
- [x] window_manager.cpp: Ctrl+U keybind calls Terrain::exportWorld()
- [x] Verify: build succeeds, --world=foo forces voxelMode, Ctrl+U in voxel mode triggers exportWorld stub

## Step 3: Export ✅
- [x] chunk.h/cpp: add getBlocks(), getBiomes(), getSegments() const-ref getters
- [x] util/file_util.h/cpp: extract getDocumentsDir() + getTimestampString() helpers
- [x] renderer_screenshot.cpp: refactored to use FileUtil helpers
- [x] terrain.cpp: implement exportWorld() — directory creation, region binary writing w/ LZ4, world.json
- [x] Verify: build succeeds, export produces valid region .bin files + world.json

## Step 3b: Structure export ✅
- [x] chunk.h/cpp: add getStructures() getter, extend loadSerializedData() to accept structures
- [x] terrain.cpp: bump region format version 1→2, add hasStructures flag bit, compressedStructuresSize field, LZ4-compressed structures payload (type + pos_WS per entry)

## Step 3c: Format v3 — checkpoint-based export ✅
- [x] terrain.cpp: bump version 2→3, gate `state >= HAS_TERRAIN`, replace flags+chunkState+segments fields with single `importLevel` byte (raw `ChunkState` enum value: 2=HAS_TERRAIN, 6=HAS_ALL_BLOCKS), drop segments payload entirely
- [x] chunk.h/cpp: change loadSerializedData signature to `(blocks, biomes, structures, importLevel)` (drop segments + state args), add `importLevel` member field on Chunk, drop unused getSegments() getter

## Step 3d: Format v4 — fix export race + strip dead code ✅
- [x] terrain.cpp: bump version 3→4, tighten gate to `state >= HAS_ALL_BLOCKS` (eliminates torn-read race documented in bug.md), drop `importLevel` byte from per-chunk record
- [x] chunk.h/cpp: replace `ChunkState importLevel` member with `bool wasImported`, drop `importLevel` param from loadSerializedData
- [x] voxel_export/plan.md: format spec v4, simplified Step 4a hooks (single `wasImported` flag)
- [x] voxel_export/bug.md: marked resolved

## Step 4: Import — chunk hooks + importWorld()
### 4a: Chunk early-return hooks
- [x] chunk.cpp: `generateTerrain` early-return inner work if `wasImported`. Always advanceState + setDirty.
- [x] chunk.cpp: `fillStructuresAndDecorators` skip the structureNeighbors loop AND decorator pass if `wasImported`. Always advanceState + run numNeighborsWithBlocks.fetch_add loop.
- [x] chunk.cpp: `checkStructureNeighbors` unchanged (always runs)
- [x] chunk.cpp: `generateSegments` unchanged (always runs)

### 4b: importWorld() ✅
- [x] settings_manager.h/cpp: add `setWorldSeed(uint32_t)` (writes both cached static + settings map)
- [x] terrain.cpp: parse world.json, fatal-exit on errors
- [x] terrain.cpp: apply worldSeed to SettingsManager BEFORE any terrain task runs; apply renderDistance only in test mode (normal import keeps user's current renderDistance)
- [x] terrain.cpp: re-call `ChunkGenerator::init()` after `setWorldSeed` (chunk_generator caches `worldSeed` + RNG-derived `noiseOffsetXZ` at init time, so initial Terrain::init's cache must be rebuilt with the imported seed before any boundary chunk runs fresh-gen)
- [x] terrain.cpp: per region — read header, validate magic + version=4, create Region, decompress per-chunk blocks+biomes (always) and structures (if size > 0), call loadSerializedData
- [x] terrain.cpp: count expected BLAS chunks (imported chunks within createBlasDistance of camera), restore camera, setDirty()
- [x] renderer.cpp: call Terrain::importWorld() right after Terrain::init in voxelMode init path (Step 5 will layer BLAS tracking around this same call site)
- [x] Verify: round-trip export→import renders correctly, no missing faces, no stuck state-machine counters (confirmed via diag log: HG=expectedBlas exactly, all imported chunks within createBlasDistance reach HAS_GEOMETRY)

### 4c: Pipeline distance fix ✅
Discovered during 4b verify with `--lockCamera=true`: chunks at `D=renderDistance` ring stuck at `HAS_ALL_BLOCKS` because their cardinal neighbors at `D+1=fillStructuresDistance+1` couldn't run `checkStructureNeighbors` (gated at `fillStructuresDistance`). Fixed by widening `fillStructuresDistance` so the structure-neighbor 5×5 footprint of every chunk inside `createBlasDistance+1` is itself within the gate.

- [x] terrain.cpp: change `fillStructuresDistance = createBlasDistance + 1` → `createBlasDistance + 1 + structureMaxChunkRadius`. `generateTerrainDistance` formula unchanged (still `fillStructuresDistance + structureMaxChunkRadius`), so it widens automatically. Work zone grows from `(2·(rd+4)+1)²` to `(2·(rd+6)+1)²` — ~12% more chunks generated, but `D=renderDistance` ring now actually renders under locked camera. Pre-existing design issue, not introduced by import work.

## Step 5: Renderer init wiring + BLAS tracking
- [x] renderer.cpp: wire importWorld() call at init (done in 4b)
- [ ] terrain.cpp: increment BLAS counter in addChunkToCreateBlas()
- [ ] terrain.cpp: implement isWorldFullyLoaded()
- [ ] Verify: log confirms all BLASes built after import (with 4c fix, `expectedBlasBuildChunks` now matches actual HG count exactly under locked camera)

## Step 6: Test loader integration
- [ ] test_loader.cpp: support "world" field in test JSON
- [ ] Verify: test harness passes --world arg correctly
