# Review: `fix_import_export` (Part 1 of `plans/chunk_streaming.md`)

Reviewed `main...fix_import_export` (7481d6d, 89e3b16, 262c94b) by reading only; tests were not built or run. 262c94b (render distance must be positive) is fine and needs no changes.

Part 1 is mostly done. Region v7 stores exact masks and ordered cave candidates. Decoding happens privately before any world state changes. Publication is checked, and reimport validates the whole world before touching the seed. The main problems are in the `RegionFile` API shape and a few assumptions that Parts 2 and 3 will run into.

## Blocking for Parts 2/3 (API shape)

1. **`RegionFile::read` returns a whole `Region`, so it can only be attached by inserting the whole object.**
   Each decoded `Chunk` stores `region` pointing at the private `Region`
   (`Region::createChunk`, `chunk.cpp:1260`). Whole-world import works because it moves
   the `unique_ptr<Region>` into an empty `regions` map. In a live world, the `Region` at
   that position usually still exists: the scan loop creates neighbor regions eagerly
   (`terrain.cpp:~415-440`), and `setNeighbors(true)` creates `NEEDS_TERRAIN` halo chunks in them.
   Regions also hold neighbor pointers to each other. A cache reload therefore can't use the
   decoded `Region` directly. It would have to re-parent chunks or swap `Region` objects under
   live neighbor pointers.
   *Suggestion:* decode to per-slot `SerializedChunkData` (e.g. `std::array<std::optional<SerializedChunkData>, 1024>`
   or `vector<pair<index, data>>`). The main thread then calls `loadSerializedData` on an existing
   `NEEDS_TERRAIN` chunk (the ASSERT already requires that state) or creates the chunk. This also
   fits the plan's "reserve loading regions" step. `write` could likewise take a span of chunk
   pointers, which would let the tests drop their hand-built `Region`s.

2. **`wasImported` means both "from an export" and "from the cache".** Cache reloads will set it through `loadSerializedData`.
   `addChunkToCreateBlas` (`terrain.cpp:142`) counts `wasImported` chunks toward the headless
   import gate, and `pollHeadlessImport` ASSERTs `enqueued <= expected`. When evicted chunks
   return in a headless run, that ASSERT will fire or the gate will count the wrong chunks. The
   pre-existing re-enqueue behavior has the same weakness. Separate "skip generation"
   (data origin) from "counts toward the import gate" before Part 3.

## Correctness / robustness

3. **The stricter writer makes export all-or-nothing on generator invariants.** `RegionFile::write` now
   requires the following for every chunk:
   - in-chunk candidate XZ
   - `0 <= y < 512`
   - `1 <= availableHeight <= 512`
   - at most 512 surface candidates
   - disjoint masks
   - `stateKind` consistency

   If any single chunk fails, the whole export aborts. With eviction, that region could never be
   written, so it could never be evicted. The generator doesn't assert these bounds when it creates
   candidates (`chunk_generator.cpp:570` ceiling anchor `layer.end`, `layerHeight`;
   `:1020` surface candidates). I didn't verify that `layer.end < chunkSizeY` always holds.
   *Suggestion:* ASSERT the invariants where candidates are created, so a violation shows up during
   development and not the first time a region is written.

4. **Details of `FileUtil::writeAtomically` that matter for the cache:**
   - The fixed `<dest>.tmp` name means two writers to one destination corrupt each other. The
     header documents this, but Part 3 has to enforce it when a region is re-evicted while its
     previous write is still in flight.
   - MSVC `std::ifstream` opens files without `FILE_SHARE_DELETE`. A background load of region X
     therefore makes a concurrent replace of X fail; the new test shows exactly this. Loads and
     writes of the same region must be serialized, not just writes.
   - A crash leaves stray `.tmp` files behind. That's harmless for the cache, which is deleted
     wholesale, but it can leave junk in `Documents/exports`.

5. **A failed export leaves an orphan directory.** The timestamped directory keeps its partial region files
   and has no `world.json`. The manifest-last ordering prevents a false "complete" export, but the
   directory is never removed. Consider `remove_all` on failure.

6. **Re-exporting a legacy world promotes its approximations to v7.** A v5/v6-imported chunk gets
   masks rebuilt from its final blocks in `generateTerrain`, and its cave list is empty. Re-exporting
   writes these as v7, which looks the same as exact data. The plan accepts this. The knowledge doc
   says v7 "preserves original masks", so it could note this exception.

## Performance / memory (relevant once writes are background)

7. **Encoding and validation cost.** `write` builds the whole region file in one `std::vector<char>`
   (every compressed chunk plus masks; plausibly tens of MB per full region, with a transient
   `LZ4_compressBound` growth per payload). It also validates every block: several passes of
   `Blocks::getBlockData` over 131k blocks per chunk. The Ctrl+U export stall on the main thread gets
   longer. For Part 3, count this buffer in the I/O memory bound, or stream payloads to the temporary
   file. Most of the per-block validation could also be debug-only, since the chunk was produced
   in-process.

## Code quality (per CLAUDE.md rules)

8. **Braceless and one-line control flow throughout the new code**, e.g.:
   - `region_file.cpp`: 55, 60, 94, 179, 194, 212-215, 237, 247, 266, 284, 319-325, 353
   - `file_util.cpp`: `if (!error) return true;`
   - `terrain.cpp` export/import: `if (!region) return false;`, `if (headless) ...`, the `none_of(...)) continue;` construct, braceless `for` in validation
   - Tests: multiple statements per line (`bad = original; put<...>(...); rejected(bad);`)

   The surrounding code, including the removed import code, uses braces consistently. Several
   lines are also well past the width used elsewhere.

9. **The `RegionFileTests` CMake target repeats a list of `Biomeinator` sources**, which will drift as
   files are added or split. An object/static library shared by both targets would be DRY. The
   target isn't registered with CTest, and `TARGET_FILE_DIR="${CMAKE_SOURCE_DIR}"` is surprising
   enough that a short comment explaining it would help.

10. **The tests are brittle in places.** The corruption cases use hard-coded byte offsets
   (4, 14, 16, 18, 26, 34, 38) that break silently if the header changes. Named offsets derived
   from the layout would be clearer. The boundary test mutates candidates through
   `const_cast` on `getCaveStructures()`. The coverage itself is good: exact round trips,
   imported-then-generate without re-decoration, both fill orders across the seam, and every
   checked-in golden region.

## Looks good

- The reimport path validates `world.json` and decodes every region before changing the seed,
  render distance, or `regions`. A failed Ctrl+O no longer leaves the world half-replaced or with
  a changed seed.
- Surface/cave candidate order and sorted block-state records make re-exports deterministic
  (tested).
- The reader bounds every allocation by the remaining file size before allocating, and rejects
  duplicates, trailing data and unused packed bits.
- The knowledge docs are updated, including the stale 5×5 → 3×3 structure-neighborhood count.
