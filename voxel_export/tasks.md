# World Export/Import — Task Tracking

## Step 1: Accessor scaffolding
- [ ] camera.h: add getPhi(), getTheta(), restoreState() declarations
- [ ] camera.cpp: implement restoreState()
- [ ] chunk.h: add loadSerializedData() declaration
- [ ] chunk.cpp: implement loadSerializedData()
- [ ] renderer.h: add restoreCamera() declaration
- [ ] renderer.cpp: implement restoreCamera() — wraps camera.restoreState()
- [ ] Verify: build succeeds

## Step 2: Settings and keybind
- [ ] settings_manager.cpp: register "world" option + voxelMode override
- [ ] terrain.h: declare exportWorld(), importWorld(), isWorldFullyLoaded()
- [ ] terrain.cpp: add empty stubs + BLAS tracking statics
- [ ] window_manager.cpp: Ctrl+E keybind calls Terrain::exportWorld()
- [ ] Verify: build succeeds, --world=foo forces voxelMode

## Step 3: Export
- [ ] terrain.cpp: implement exportWorld() — directory creation, timestamp naming
- [ ] terrain.cpp: implement region file writing with LZ4 compression
- [ ] terrain.cpp: implement scene.json writing
- [ ] Verify: Ctrl+E in voxel mode produces valid export folder

## Step 4: Import
- [ ] terrain.cpp: implement importWorld() — JSON parsing, region file reading
- [ ] terrain.cpp: implement LZ4 decompression + chunk population via loadSerializedData()
- [ ] terrain.cpp: apply renderDistance/worldSeed from JSON
- [ ] terrain.cpp: count expected BLAS chunks, restore camera, setDirty()
- [ ] Verify: round-trip export→import renders correctly, no missing faces

## Step 5: Renderer init wiring + BLAS tracking
- [ ] renderer.cpp: wire importWorld() call at init
- [ ] terrain.cpp: increment BLAS counter in addChunkToCreateBlas()
- [ ] terrain.cpp: implement isWorldFullyLoaded()
- [ ] Verify: log confirms all BLASes built after import

## Step 6: Test loader integration
- [ ] test_loader.cpp: support "world" field in test JSON
- [ ] Verify: test harness passes --world arg correctly
