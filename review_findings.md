# Branch review findings

Reviewed branch: `reduce_vram_usage` at `09e9000`.
Comparison base: `main` at `4dfa024`.

This review covers correctness, duplication, unnecessary or misleading code and
documentation, and useful defensive assertions. The review itself was read-only;
this document records the findings for the agent doing follow-up work. Source line
numbers below refer to the reviewed commit and may move as fixes are made.

## Owner clarification: packed positions may be intentional

The owner believes the difference between BLAS positions and packed shading
positions may be intentional: saving resident vertex memory in exchange for very
slight positional inaccuracy. They do not remember for sure.

The follow-up agent should investigate this intent before changing the design.
Do not treat finding 1 as an instruction to automatically undo vertex packing or
restore full-precision resident vertices. If the tradeoff is intentional, add or
expand source comments and relevant knowledgebase entries to explain it clearly:
why the two representations differ, the expected error bounds, which geometry is
affected, and why the resulting shading and ray-origin behavior is acceptable.
Useful places include `knowledge/shaders/common_structs.md`,
`knowledge/gpu/acceleration_structures.md`, and `knowledge/scene/instance.md`.
Follow the project's knowledgebase editing rules in `CLAUDE.md`.

The numerical discrepancy below is confirmed; its visual severity and whether it
is an accepted tradeoff still need renderer validation. The original P1 assessment
for finding 1 should therefore be revisited in light of this clarification.

## Correctness findings

### 1. BLAS and shading positions differ enough to permit self-intersection

Original priority: **P1**, subject to the owner clarification above.

Primary location: `src/rendering/buffer/acs_helper.cpp:415-419`.
Related code: `src/util/packing.h`,
`src/shaders/common/path_tracing_common.hlsli:255-266`, and
`src/shaders/util/ray.hlsli`.

The BLAS uses the original fp32 positions, while `ClosestHit_Primary` reconstructs
hit positions and geometric normals from quantized resident vertices. Existing
crystal and glowshroom meshes are not confined to the exact grid described in the
packing comments. Their Y rounding error reaches approximately 0.0073 blocks.

A read-only numerical check of the checked-in GLB assets placed their geometry at
offset `(0.5, 128, 0.5)`, applied the packing quantization, reconstructed triangle
centroids and normals, and applied the shader's ray-origin offset. Rays leaving
the rounded surface could still intersect the original triangle. Checking both
sides found such cases on 21 of 36 crystal triangles and 39 of 108 glowshroom
triangles; the unjittered brown mushroom had none. These are numerical examples,
not measured on-screen artifact rates or a GPU rendering test.

This can produce self-shadowing or repeated hits, rather than only a small
displacement of the shaded surface. Secondary and shadow rays use `TMin = 0`.

Follow-up:

- Establish whether this is an intentional, accepted memory/accuracy tradeoff,
  and document the rationale as requested above.
- Validate the affected assets and jittered foliage in the renderer, including
  close views and secondary/shadow rays.
- If the effects are unacceptable, consider building the BLAS and area lights
  from the same decoded positions, or retaining matching precision for affected
  geometry. Evaluate the visual and memory consequences before choosing a fix.

### 2. Pending compaction records can dereference destroyed instances

Priority: **P1**.

Primary location: `src/scene/scene.cpp:549-556`.
Related code: `Scene::requestNewInstance`, `Scene::freeInstance`,
`Renderer::flush`, and `Terrain::resetTerrainState`.

The build-ID check already dereferences a potentially dangling `Instance*`.
`Renderer::flush()` frees every frame's deletion list, while pending compaction
records remain. `freeInstance()` moves an old object into `instancesToReuse`;
`requestNewInstance()` then steals its vectors and pops the queue, destroying the
old object. The object itself is not kept alive or recycled in place.

A possible sequence is:

1. Build an instance and queue its compaction result in one frame slot.
2. Schedule the instance for deletion before that slot is revisited.
3. Flush during a resize or screenshot, freeing the instance early relative to
   consumption of the pending compaction record.
4. Allocate another chunk, stealing the old vectors and destroying the object.
5. Revisit the compaction slot and read the dangling pointer.

World reimport also flushes and immediately frees terrain instances without
clearing the scene's pending compaction records. The same lifetime concern applies
if replacement allocations destroy those objects before their records are read.

Store an instance ID plus build generation and resolve it through the live
instance map before dereferencing, or invalidate pending records when instances
are freed. A build-generation comparison through a raw pointer does not establish
that the pointed-to object is still alive. This finding comes from tracing the
code's lifetime paths; a runtime crash was not reproduced during the review.

### 3. The random walk never initializes its first random heading

Priority: **P2**.

Primary location: `src/rendering/renderer/renderer_perf.cpp:259-265`.
Related call ordering: `src/rendering/renderer/renderer.cpp:600-655`.

`perfRunPlayerInput()` runs before `perfRunUpdate()` enters `MEASURING`, so
`perfRunMoveDirection()` first sees `measuredFrame == 1`. For turn intervals
greater than one, the frame-zero heading is never generated. Movement stays at
the default forward direction until the first interval boundary.

With `perfMoveTurnFrames >= perfFrames`, the entire measured path is straight,
despite requesting random-walk movement. A heading selected on the frame that
ends measurement does not repair the measured path.

Deriving the heading directly from the interval index would initialize it
correctly and remove the cached `moveDirection` state. Alternatively, initialize
the heading explicitly when entering the measuring phase.

## Cleanup and deduplication

### Consolidate compaction records

Location: `src/scene/scene.h:168-172`.

`PendingBlasCompaction` maintains parallel instance and geometry-entry vectors
describing the same builds. One record per pending build would remove ordering
and clearing bookkeeping. Incorporate safe identity/lifetime handling from
finding 2 rather than merely merging the raw pointers into one struct.

### Centralize scene SRV ordering

Locations: `src/rendering/renderer/renderer_internal.h` parameter enums,
`src/rendering/renderer/renderer_pipeline.cpp` root-signature construction, and
`src/rendering/renderer/renderer.cpp:359` (`bindSceneSrvs`).

The packed-vertex binding extends matching lists in both parameter enums, both
root signatures, and the binding function. A shared scene-SRV layout definition
would prevent these lists from drifting. Keep the abstraction small and focused
on this repeated ordering.

### Remove unused counters

Location: `src/scene/scene.cpp:713-752`.

`numPerFaceDatas` and `numAreaLights` are accumulated but never read in
`makeQueuedBlases()`. Remove the variables and their increments. These are
existing cleanup opportunities in the touched code, not new functional failures.

### Correct misleading explanations

Location: `knowledge/gpu/acceleration_structures.md:81-92`.

The compaction notes claim that glTF builds everything in one batch, but glTF
instances enter the same capped build queue. The notes also describe instance
reuse without explaining that the implementation steals vectors and destroys
the old object. These confident but inaccurate explanations are the clearest
documentation cleanup opportunities from the review.

Also reconcile the packing comments' exact-grid explanation with the existing
tilted custom meshes and jitter. If their inaccuracy is deliberate, explain that
tradeoff instead of implying that every model position decodes exactly.

## Defensive assertions

### Validate complete, ordered emissive faces

Location: `src/scene/scene.cpp:162-165` (`Instance::addAreaLights`).

For paired terrain triangles, the current assertion does not establish the stated
invariant that every triangle in an emissive face has a consecutive light entry.
For example, a supplied triangle list containing only `[0]` passes, but shader
recovery maps triangle 1 to light index 1 even though no such light was added.
A list containing only `[1]` can underflow the stored base index to
`LIGHT_IDX_INVALID` and also passes the assertion.

Validate complete groups and ordering, with explicit handling for the degenerate
padding triangle used by odd-triangle custom models. The current production
callers were not shown to supply these malformed lists; this is a missing
defensive check demonstrated with small input examples.

### Reject zero compacted sizes

Location: `src/rendering/buffer/acs_helper.cpp:489-493`.

Assert `compactedSizeBytes > 0` before allocation and copy. Zero currently passes
the upper-bound assertion and reaches a zero-size allocation/copy path. It is
better to catch an invalid query result at this boundary.

### Pin CPU/GPU layouts and shared vertex alignment

Location: `src/rendering/common/common_structs.h`, with the allocation alignment
configured in `src/scene/scene.h`.

Add compile-time checks for `sizeof(PackedTerrainVertex) == 12` and
`sizeof(PerFaceData) == 8`. Also assert that the shared vertex-buffer allocation
alignment is compatible with both vertex strides. Currently its 24-byte alignment
works for both 24-byte full vertices and 12-byte packed vertices; a future layout
change could silently break byte-offset-to-element-index conversion.

### Bound the face-size shift

Location: `src/scene/scene.cpp:221` (`Instance::setTrisPerFaceLog2`).

Assert the supported range before later `1u << trisPerFaceLog2` operations. At a
minimum require a value below 32; if only triangles and pairs are supported by
design, require 0 or 1 and document that restriction.

## Validation performed

- Static review of the branch diff and relevant callers, lifecycle code, shaders,
  and knowledgebase entries.
- In-memory numerical checks against the existing custom-model GLBs, plus small
  checks of the emissive-face assertion and random-walk interval logic.
- `git diff --check main...HEAD` passed at the reviewed commit.
- No renderer build, GPU run, screenshot comparison, or runtime crash reproduction
  was performed. No implementation files were changed by the review.
