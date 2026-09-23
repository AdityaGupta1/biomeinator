_Last edited: 2026-09-22_

# Region file regression tests

`RegionFileTests` is a CPU-only target using the production region codec, Chunk
methods, generator, structure/decorator passes, and block JSON loader. Rendering
and model mesh loading are omitted; these checks require neither a GPU nor an
application window. Build with the normal optimized, asserted configuration:

```
cmake --build build --config RelWithDebInfo --target RegionFileTests
build/RelWithDebInfo/RegionFileTests.exe
```

Artifacts live under `build/region_file_tests/`. The checked-in golden worlds are
read-only inputs. The tests intentionally emit error logs for rejected malformed
files and simulated write failures; the final exit status determines success.

The important distinction from screenshot tests is that the CPU suite compares
generation inputs as well as final blocks. A decorated block cannot tell us whether
its cell was originally terrain air. Exact mask/candidate round trips, followed by
the imported chunk's real generation callbacks, catch accidentally rebuilding those
inputs or stamping structures twice.

Boundary checks export completed chunks while their neighbors are unfinished, then
reload, regenerate those neighbors, and compare them with uninterrupted generation.
The scene crosses the negative/positive region seams, uses several fixed seeds, and
injects a lamp candidate in upper air spanning the saved/fresh boundary. This makes
the dependency on cave candidates and original air masks observable regardless of
which natural cave biome happens to appear at the origin. Different completion
orders must give the same results.

Legacy fixtures exercise v5 upward-facing block-state migration and v6 explicit
orientations, plus every region in the checked-in voxel worlds. Missing original
generation inputs in those formats deliberately use the historical approximations.
See [world export/import](../terrain/world_export_import.md).
