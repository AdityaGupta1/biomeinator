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
- [x] Verify: build succeeds, --world=foo forces voxelMode

## Step 3: Export ✅
- [x] chunk.h/cpp: add getBlocks(), getBiomes(), getSegments() const-ref getters
- [x] util/file_util.h/cpp: extract getDocumentsDir() + getTimestampString() helpers
- [x] renderer_screenshot.cpp: refactored to use FileUtil helpers
- [x] terrain.cpp: implement exportWorld() — directory creation, region binary writing w/ LZ4, scene.json
- [x] Verify: build succeeds, export produces valid region .bin files + scene.json

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
