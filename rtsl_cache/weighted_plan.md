# RTSL Tile Cache — Per-Light Success-Rate Weighting Plan (v2)

_Last edited: 2026-05-23_

## Changelog

- **W3 done (2026-05-23):** outcome attribution. `tcLookupReprojected` gained an
  `out uint prevSlotBase` (slot-0 index of the reprojected Prev cell) so the
  sampler can read a chosen slot's counters with no second reprojection — two
  extra `Load`s on the accept path only; the pdf passes a throwaway. New
  `TileCacheSeed` (valid / lightIdx / decayed attempts+successes) + `tcSeedNull`
  in `tile_cache_cells.hlsli`. `lcSelectSubtreeRoot` gained an `out TileCacheSeed
  seed` (valid only on the accept path — false on uniform branch / miss / rejected
  slot); the seed thread-through draws no extra RNG, so the disabled path stays
  byte-identical. `tcInsert` **replaced by `tcUpsert`** (find-or-add + atomic
  counter accumulate): Pass 1 does a read-only find scan for the light FIRST, then
  CAS-claims the first empty slot only on a miss — finding a resident copy before
  claiming an empty slot stops one light's counters being split across two slots
  when an empty slot precedes the resident one (a low slot freed by the carry's
  dead-light drop); the single-CAS-pass form duplicated the light there (review
  finding M1, folded in — variance-only today but corrupts the rate W4 reads). A
  freshly claimed slot is seeded from X's carried counters via `InterlockedAdd`
  (carry guarantees a claimed slot starts (0,0), so seed+delta composes with
  racing dup-adds); only the residual two-first-inserters race can still
  duplicate, bounded + self-heals next carry. Pass 2 (cell full) still
  picks a **uniform** victim — one RNG draw, matching the base plan — but
  CAS-claims it on a bounded same-slot retry instead of a tearing `Store` (audit
  fix #4 brought forward because counters are now persistent). `sampleDirect-
  LightingRtsl` now also outs `shadowRayCast` / `unoccluded` / `resolvedLightIdx`
  so the occlusion result is visible to the caller (the early-out returns hid it).
  `path_tracing.rgs.hlsl`: the cache write moved **out** of the `didHitLight`
  guard (audit fix #1 — attempts must count occluded samples or the rate is dead
  at ≡1); X is upserted (+1 attempt, +hit success, seeded from carried counters),
  Y gets a neutral (0,0) membership vote (skipped when X==Y or occluded), and a
  bare uniform/miss hit still votes Y in to bootstrap cold cells. The whole write
  block is gated `#if !NRC_UPDATE` (audit fix #3 — sparse UPDATE pixels would
  contaminate the additive rate; the pdf still reads Prev in both builds). Counters
  are **populated but not yet read** by eviction (Pass 2 stays uniform; W4 swaps in
  the weighted min-scan). Build clean; representative goldens within float drift of
  W2 (`cornell_box_rtsl` 0.00896, `cave_lights` 0.00537, `two_triangles` pass);
  `test_cache_mis.py` 11/11 + neg control trips (sampling law untouched — the seed
  out-param is pure observability). **Signal liveness (validation item 6) holds by
  construction** — `attempts` increments unconditionally outside the `didHitLight`
  guard, `successes` only when unoccluded — but the empirical per-slot dump it asks
  for needs debug-readback tooling that does not exist yet (base-plan step 8.3's
  `dev_dbgRtslSamples` / `analyze_cache_hist.py` were never built); deferred to W5.
- **W2 done (2026-05-23):** carry pass replaces the per-frame clear. New
  `rtsl_tile_cache_carry.cs.hlsl` (one thread per cell) reprojects Prev→Curr by
  tile-center motion vector, decays both counters (`round(c * statDecay)` via
  `tcPackCounter`), and drops dead lights; rejects (→ fully empty cell) on
  `suppressPrev`, `frame 0`, `prevUv` OOB, or center-depth disocclusion.
  **Carry root sig is GLOBAL_PARAMS CBV + `rtslLightToLeaf` root SRV, not
  "GLOBAL_PARAMS only"** — lightToLeaf is root-SRV-bound (not in the descriptor
  heap), so the dead-light check binds it the way PT does; the tile-cache
  buffers / motion / linear-depth are still reached bindless via the ping-ponged
  `heapIndices`. Refactor: the light-tree-INDEPENDENT half of `tile_cache.hlsli`
  (cell addressing, sub-bucket, counter pack/rate, slot load, reprojection,
  new shared `tcDepthRejects`) was split into `tile_cache_cells.hlsli` so the
  carry can reuse it without dragging in the `lc*` sampler/pdf (which reference
  `rtslLightTree` / `evaluateLightSelectPdf` the carry never binds); no `lc*`
  body changed. `renderer.cpp`: `linearDepthCurr` UAV→NPSR before the carry
  (audit fix #2 — carry is its first SRV reader) and restored NPSR→UAV after PT
  so the DLSS-D tag still matches; clear dispatch swapped for the carry; UAV
  barrier on Curr and the one-shot init clear kept. Build clean; representative
  goldens within float drift of step 6 (`cornell_box_rtsl` 0.00895,
  `cave_lights` 0.00531, `two_triangles` pass); `test_cache_mis.py` 11/11 + neg
  control trips (header split left the estimator math untouched). Three parallel
  reviews (correctness, GPU/dispatch, edge-cases): no critical findings.
- **W1 done (2026-05-23):** slot layout + params plumbing. `RTSL_TILE_CACHE_SLOT_BYTES`
  8 → 16; added `RTSL_CACHE_STAT_SCALE` (256), `RTSL_CACHE_COUNTER_MAX` (clamp
  ceiling, exactly float-representable so `tcPackCounter`'s round-trip can't
  overflow), and `RTSL_TILE_CACHE_CARRY_WORKGROUP_SIZE`. Buffer alloc + slot stride
  both derive from `SLOT_BYTES`, so they doubled with no extra edits. Clear shader
  zeros the two new words. `RtslCacheParams` gained `statDecay`/`evictPriorStrength`
  (+ 2 pads, struct now 48 B); uploaded in `renderer.cpp`. Two settings registered;
  GUI sliders deliberately omit `didPathTracingSettingsChange` (arithmetic-only —
  see Settings). `tile_cache.hlsli` gained `tcPackCounter` (round + clamp) and
  `tcCounterRate`; counters written 0, not yet read. Build clean; representative
  goldens pass within float drift of step 6 (`two_triangles` 0.00080,
  `cornell_box_rtsl` 0.00896, `cave_lights` 0.00532).
- **v2 (2026-05-23):** audit-driven revision (three parallel reviews: correctness,
  GPU/dispatch, edge-cases). Critical fixes folded inline:
  1. **Outcome must be recorded OUTSIDE the `didHitLight` guard.** The existing
     insert is success-only (occluded NEE returns before touching the cache), so
     recording there would give `attempts == successes`, `rate ≡ 1`, and a dead
     learning signal that the bias-only tests cannot detect. Record the attempt
     unconditionally, success conditionally. Added a signal-liveness golden.
  2. **Carry pass needs a `linearDepthCurr` UAV→NPSR transition.** Step 6 leaves
     it in UAV (PT computes curr depth via `distance()`, never reads the texture);
     the carry is the first SRV reader.
  3. **NRC: gate counter/outcome WRITES to `!NRC_UPDATE`.** Both UPDATE and QUERY
     run the insert in one frame; with additive counters the sparse UPDATE pixels
     contaminate the rate. The pdf still reads Prev identically in both builds, so
     the NRC consistency rule (about the *pdf*) is preserved.
  4. **Eviction claims the victim with CAS, then writes counters.** A non-atomic
     `Store` would tear the 16 B slot against concurrent `InterlockedAdd`s.
  5. **Outcome attributed to the insert cell, with X inserted if absent.** The
     `mayEvict=false` drop systematically lost boundary-pixel signal; X is now
     upserted (real delta beats prior) so the outcome always lands and doubles as
     per-pixel carry for seeded lights.
  6. **Carry drops dead lights** (`rtslLightToLeaf == INVALID`) so a removed
     light's high counters can't squat a slot for the decay window.
  7. Fresh-light prior is now **relative to the cell** (not absolute 0.5);
     eviction ties broken by attempts. Counter store rounds (not floors) and
     clamps. Stat-only settings no longer wire `didPathTracingSettingsChange`.
- **v1 (2026-05-23):** initial plan. Builds on the screen-space tile cache from
  `plan.md` (steps 1–6 DONE). Adds persistent per-light visibility statistics and
  success-rate-weighted eviction.

## Relationship to the base plan

`plan.md` describes the screen-space tile cache: per-tile × depth-sub-bucket cells
of `K` slots, each slot `(lightIdx, normalTag)`. Reads reproject to the previous
frame's cell; inserts vote the NEE-hit light into the current frame's cell; the
cell is **cleared every frame** and rebuilt. Sampler (`lcSelectSubtreeRoot`) and
emission-MIS pdf (`lcEvaluateMixturePdf`) read the identical Prev cell, which is
what keeps the estimator unbiased.

This plan changes two things:

1. **Cells persist across frames** via a per-frame **carry pass** (reproject +
   decay) that replaces the per-frame clear. Light membership and per-light
   statistics accumulate over time instead of being rebuilt from scratch.
2. **Per-light success statistics** (visibility) travel in the slot payload and
   drive **weighted eviction**: when a full cell must drop a light to admit a new
   one, it evicts the light least likely to yield an unoccluded sample.

## Goal

Stop wasting cache capacity on lights that are consistently occluded from a tile.
The light tree selects by power/distance and is visibility-blind; the cache can
*learn* per-region visibility from NEE shadow-ray outcomes and prefer to retain
seeds whose subtree descents actually reach light. Net effect: lower NEE variance
(faster convergence) in scenes with local occlusion, with no bias.

## Scope and non-goals

- **Weighted eviction only.** Slot **selection in the sampler stays uniform**
  (`subSlot = rng * K`). The mixture pmf is therefore unchanged, so
  `lcEvaluateMixturePdf` is untouched and MIS stays valid by the same argument as
  the base plan. *Weighted selection is a deliberate follow-up*, not this plan.
- **No ReSTIR-style spatial or temporal reuse.** No reservoirs, no resampling.
  The only temporal mechanism is the carry pass (decayed reprojection of the
  cell), which is the same temporal model the base plan already uses for reads.
- **Primary diffuse hit only**, identical gate to the base plan
  (`useRtsl && pathDepth == 0 && pathSplitIdx == 0 && rtslCacheParams.enabled`).

## Locked design decisions

| Decision | Choice |
|---|---|
| Cell persistence | Carry pass (reproject + decay), replaces per-frame clear |
| Carry reprojection | v1: **tile-center** motion vector. Planned refinement: per-sub-bucket in-tile probe (see Carry pass) |
| Statistic | `attempts`, `successes` (failures = attempts − successes) |
| Temporal model | **EWMA** — multiplicative decay applied each carry |
| Storage of counters | uint fixed-point (scale `RTSL_CACHE_STAT_SCALE`), rounded + clamped, read as float |
| Fresh-light prior | mean = cell's current rate (0.5 when empty), strength = `priorStrength` |
| Eviction | victim = lowest confidence-shrunk rate; CAS-claimed; ties by attempts |
| Counter writes | only on `!NRC_UPDATE` dispatches (pdf still reads in both) |
| Outcome event | recorded outside the `didHitLight` guard (attempt always) |
| Sampler selection | unchanged (uniform) → MIS math unchanged |

### Why uint fixed-point instead of raw float

EWMA wants fractional counts, which suggests `float`. But event increments are
**concurrent** (many primary-hit samples in a tile seed off the same slot), and
HLSL has no portable float `InterlockedAdd` (only an NVAPI extension). Losing
increments to non-atomic float races would systematically undercount and corrupt
the rate. Resolution:

- Store `attempts`/`successes` as `uint`, scaled by `RTSL_CACHE_STAT_SCALE`
  (e.g. 256) so the **decay multiply keeps fractional precision** at low traffic.
- The decay multiply happens **only in the carry pass**, which is the exclusive
  writer of a Curr cell that frame — a plain float multiply, stored back as uint.
  No atomic needed there.
- Per-sample event increments use integer `InterlockedAdd(scale)` /
  `InterlockedAdd(scale * hit)` — atomic and exact.
- `rate = float(successes) / float(max(attempts, 1u))` — scale cancels; the
  `max` guards the `attempts == 0` fresh slot.
- The carry-pass decay **rounds** (`uint(x * decay + 0.5)`), not floors, so the
  effective decay matches the nominal `decay` instead of biasing steeper at low
  counts, and a once-seen slot doesn't silently collapse to "fresh."
- Counters are **clamped** at store to keep the EWMA fixed point in range:
  steady state is `attempts* ≈ scale · a / (1 − decay)` where `a` = samples that
  hit one slot in a frame (≤ tile-pixels × spp). Confirm `attempts*` fits u32 at
  the max supported spp; clamp defensively at store regardless.

(Float storage + NVAPI `NvInterlockedAddFp32` is a fallback if precision ever
proves insufficient; recorded under Risks, not chosen.)

## Data layout

Slot grows from 8 B to **16 B**:

```
u32 lightIdx       // unchanged
u32 normalTag      // unchanged (full 32-bit oct)
u32 attempts       // fixed-point, scale RTSL_CACHE_STAT_SCALE
u32 successes      // fixed-point, scale RTSL_CACHE_STAT_SCALE
```

`RTSL_TILE_CACHE_SLOT_BYTES` 8 → 16. Memory: ~66 → **133 MB** per buffer pair at
1080p, ~530 MB at 4K — within the headroom the base plan already accepted.
Layout stays 4-aligned; the RAW UAV/SRV element count doubles.

New constants (`common_settings.h`):
- `RTSL_CACHE_STAT_SCALE` — fixed-point scale for counters (256).
- `RTSL_TILE_CACHE_CARRY_*` workgroup sizing (mirrors the clear constants).

## Per-frame cell lifecycle (the core change)

Replaces step-6's `clear → barrier → PT`:

```
1. G-buffer (writes linearDepthCurr UAV, motion)          [unchanged]
2. Transition linearDepthCurr UAV → NPSR                  [NEW — carry reads it as SRV]
3. Carry pass (compute): Prev → Curr, per cell            [REPLACES clear]
4. UAV barrier on tileCacheCurr                           [carry → PT write]
5. Path tracing: reads Prev (seed) + writes Curr          [+ outcome upserts]
```

**`linearDepthCurr` state (audit fix #2).** Step 6 leaves `linearDepthCurr` in
UNORDERED_ACCESS after the gbuffer write — PT computes current depth via
`distance()` and never reads the texture, so nothing transitioned it. The carry
pass is the **first** consumer that reads `linearDepthCurr` as an SRV (for the
disocclusion test), so it needs an explicit UAV → NON_PIXEL_SHADER_RESOURCE
transition before the carry. Restore to its step-6 end state afterward (the
existing pre-postprocess `linearDepthCurr → PIXEL_SHADER_RESOURCE` still applies;
just ensure the tracked state is consistent). `linearDepthPrev` and `motion` are
already moved to NPSR by step 6.

**Runs once per frame, in all NRC modes.** The carry is a single compute dispatch
before `dispatchPathTracing`, independent of NRC. The NRC UPDATE+QUERY
double-write concern is about *counter writes during PT*, handled in Outcome
attribution, not the carry.

### Carry pass

One thread per Curr cell (or per slot; sizing TBD like the clear). For Curr cell
`(tile, subBucket)`:

1. `centerPixel = tile * RTSL_TILE_PIXELS + RTSL_TILE_PIXELS/2`.
2. `motion = motionTex[centerPixel]`; `prevUv = centerUv + motion`.
3. Reject → write an **empty** cell (all four words: lightIdx = INVALID, normalTag
   = 0, attempts = 0, successes = 0) when: `suppressPrev`, `frameNumber == 0`,
   `prevUv` out of bounds, or the center depth disocclusion test fails
   (`|currDepth − prevDepth| / max(...) > rejectDepthRel`, reusing the read-path
   logic and thresholds).
4. Otherwise copy the Prev cell at `(prevTile(center), subBucket)` — **same
   sub-bucket index**, matching the read convention ("sub-bucket computed from
   current depth, looked up on the prev tile"). For each slot:
   - **Drop dead lights (audit fix #6):** if `lightIdx == INVALID` or
     `rtslLightToLeaf[lightIdx] == LEAF_IDX_INVALID` (the same predicate
     `tcSlotAccepts` uses), write an empty slot. Otherwise a light removed from the
     tree keeps its high counters and squats a slot for the whole decay window,
     and an index reused for a new light carries the old light's stats.
   - Else copy `lightIdx`, `normalTag`; `attempts = round(attempts_prev * decay)`,
     `successes = round(successes_prev * decay)` (round, not floor — see Data
     layout). Every empty/copy path writes all four words.

Tile-center reprojection means one source cell and one disocclusion test per tile,
applied to all four sub-buckets. Coarse but cheap.

#### Per-sub-bucket reprojection (planned refinement)

The single tile-center reproject is wrong for **silhouette tiles** — a tile
straddling a depth discontinuity has its center on one surface (say foreground),
so the *other* sub-buckets (background) are carried from the foreground's
reprojected location, pulling in the wrong scene region's lights for those bands.
It's only a variance issue (the read path still depth-rejects per pixel and falls
back to root descent), and silhouette tiles are a minority — hence tile-center is
the v1 default.

The carry dispatch is **one thread per cell** = one thread per `(tile,
subBucket)`, which makes a per-band fix natural: each cell thread probes pixels
*within its tile* for one whose `tcDepthBucket(depth) == subBucket`, then uses
**that** pixel's motion vector + depth for the reproject and disocclusion test.
Bands with no matching pixel → write an empty cell (correct: no surface in that
band → nobody reads it). Cost is an in-tile depth probe per cell:

- Full 8×8 scan (64 taps/cell) — exact, heaviest.
- Sparse probe (e.g. an 8-tap stride) — cheap; may miss a band occupying very few
  pixels, in which case that cell falls back to tile-center or empty.

This is a localized accuracy win for depth edges at the cost of carry-pass depth
fetches. Deferred to after W4 so it can be measured against the convergence test;
upgrade only if edge tiles show poor persistence.

Resources the carry pass binds: `tileCachePrev` SRV, `tileCacheCurr` UAV,
`motion` SRV, `linearDepthCurr` SRV, `linearDepthPrev` SRV, plus `rtslLightToLeaf`
(dead-light check). **Root signature = `GLOBAL_PARAMS` only**, heap-direct-indexed
(audit fix). Unlike the clear — which used three per-dispatch root constants so one
PSO could clear both A and B in one command list — the carry runs **once per frame
on the parity-selected Curr**, so it can read everything from the already-
ping-ponged `heapIndices` / `rtslCacheParams` (decay, `rejectDepthRel` reused for
read/carry symmetry) like the gbuffer and PT passes do. No root-constant trick.

## Outcome attribution (seed = X)

Per primary-hit NEE sample, the cache touches two lights: the **seed X**
(`lk.slots[subSlot]`, the cached light whose `L`-ancestor subtree the descent
starts from) and the **resolved leaf Y** (what `selectLightFromSubtree` actually
samples and the shadow ray tests). The shadow-ray outcome is credited to **X** —
"did seeding off X from this surface region reach light?" — because eviction acts
on the cached entry X, not on Y. The quantity learned is really the visibility of
X's `L`-ancestor cluster from this tile.

### Record on every seeded sample, not just hits (audit fix #1 — CRITICAL)

The existing insert lives inside `if (lightSample.didHitLight)`
(`path_tracing.rgs.hlsl:329`), and `didHitLight` is true **only when the shadow
ray was unoccluded** — occluded NEE returns earlier and never reaches the cache.
So a rate recorded at that site would be `successes == attempts` always, `rate ≡
1`, and weighted eviction would learn nothing — invisibly, because the bias-only
tests never fire on it.

Therefore the NEE branch must be restructured so the **occlusion result is visible
at the record site**, and the outcome is recorded for every sample that *drew a
seed and cast a shadow ray*:
- `attempts += 1` unconditionally (a shadow ray was cast for X's descent).
- `successes += 1` only if unoccluded.
- A sample that took the uniform branch, missed the cache, or where
  `selectLightFromSubtree` failed before casting a ray → **no event** (no
  shadow ray, no seed).

### Threading

- `lcSelectSubtreeRoot` gains an `out` seed descriptor: X's `lightIdx` and X's
  **decayed Prev counters** (read while the sampler already has the Prev slot in
  hand — no extra fetch), or a sentinel when the sample took the uniform branch /
  missed.
- Inside the same `if (useRtsl && atPrimaryHit && rtslCacheParams.enabled)` block
  as the insert (so it stays on the `pathSplitIdx == 0` lane), after the occlusion
  trace, path tracing records the outcome.

Events come **only** from the NEE sampler. The MIS-pdf side reads the cache but
samples nothing → no events.

### NRC: only QUERY/normal dispatches write counters (audit fix #3)

In NRC builds, `dispatchPathTracing` runs the NRC_UPDATE dispatch (sparse
training-dim pixels) **and** the NRC_QUERY dispatch (full render-dim) in the same
frame, both into `tileCacheCurr`. With additive counters, UPDATE's sparse,
differently-distributed pixels contaminate the rate QUERY relies on. So **gate all
counter/membership writes** (`tcUpsert`, the carry pass already runs once) behind
`#if !NRC_UPDATE`. The UPDATE dispatch still *reads* Prev for an identical pdf, so
the base plan's NRC consistency rule — which is about the **pdf** — is preserved;
only statistic mutation is QUERY-only.

### Attribute to the insert cell, inserting X if absent (audit fix #5)

The outcome credits X's visibility **from this pixel's surface region** =
this pixel's Curr cell `(currTile, subBucket)` (the same cell the Y vote targets).
X must be resident there to receive the update. Because the carry source
(tile-center) and the seed source (per-pixel) differ at tile boundaries, X is
*not* always carried into this cell — and a `mayEvict=false` skip would
systematically drop boundary-pixel outcomes (a persistent, not transient, loss
under camera motion). So X is **upserted with eviction allowed**, seeding its slot
from the decayed Prev counters in the seed descriptor plus this event. This makes
the outcome always land and doubles as an accurate **per-pixel carry** for seeded
lights (complementing the coarse tile-center carry pass).

### Upsert

```
tcUpsert(currCell, lightIdx, dAttempts, dSuccesses, seedCounters, normalTag)
```

- Scan the cell for `lightIdx`. If found → `InterlockedAdd(dAttempts, dSuccesses)`
  onto its counters.
- If absent → claim a slot via Pass-1 (dup/empty CAS) then Pass-2 (weighted,
  CAS-claimed eviction — see below). Initialize the new slot's counters to
  `seedCounters + (dAttempts, dSuccesses)`.

Call pattern per sample (X = seed, Y = resolved leaf):
- **X (seed outcome):** `tcUpsert(X, +1, +hit, seedCounters = X's decayed Prev
  counters, tag)`. A real observed outcome; on a fresh insert it seeds from X's
  carried history, **not** the neutral prior.
- **Y (membership vote):** `tcUpsert(Y, 0, 0, seedCounters = neutral prior, tag)`.
  Keeps the sampled light resident; a fresh Y starts neutral (no observed outcome
  for Y itself — the ray tested Y's visibility but we attribute it to the seed X).

When `X == Y` (descent resolved back to the seed), do the **X** call only — the
real outcome must take precedence over the neutral-prior Y vote. The find scan in
the Y call would otherwise no-op (X already resident), but skipping it explicitly
avoids re-seeding a just-inserted slot with the prior.

### Find-then-add atomicity (audit note)

The scan-find then `InterlockedAdd` is not a single atomic op: a concurrent
Pass-2 eviction in the same cell could replace the found slot's light between the
find and the add, miscrediting the new occupant. This only perturbs variance
(never bias), is bounded (Pass 2 is rare), and self-heals next carry. Accepted; if
the W5 histogram shows meaningful miscrediting, re-validate the lightIdx word
after the add and discard on mismatch.

## Weighted eviction + confidence

Pass 2 fires only when a cell is full and the incoming light has no dup/empty
slot. Victim = slot minimizing the **confidence-shrunk success rate**:

```
rate_est = (successes + priorSuccesses) / (attempts + priorAttempts)
priorAttempts  = RTSL_CACHE_STAT_SCALE * priorStrength      // in effective attempts
priorSuccesses = priorMean * priorAttempts
```

**Prior relative to the cell (audit fix #7).** An *absolute* 0.5 prior is only
"mid-pack" when the cell's residents straddle 0.5. In a well-lit cell every
resident converges to `rate_est → 1`, so a fresh light at 0.5 is the **worst** and
gets thrashed out immediately — the exact failure the prior was meant to prevent;
in a fully-occluded cell a fresh 0.5 light squats. So `priorMean` is the cell's
**current mean (or max) resident rate**, computed in the same min-scan, so "fresh"
means "typical for this cell." Falls back to 0.5 for an all-empty cell.

- A consistently occluded light drives `rate_est → 0` → first evicted.
- A genuinely fresh light sits at the cell's typical rate → survives long enough
  to earn its own.
- **Ties broken by `attempts`:** prefer evicting the lower-evidence slot, so a
  light that has *earned* a mid rate beats an untested newcomer at the same rate.

**CAS-claimed eviction (audit fix #4).** A plain `Store` of the new
`lightIdx + 2 counter words` would tear against concurrent `InterlockedAdd`s from
other lanes still crediting the victim. Instead, claim the victim with an
`InterlockedCompareExchange` on its `lightIdx` word (old victim id → new id); only
the winning lane then writes the new `normalTag` + initial counters. A lane whose
CAS loses re-scans (the cell changed) or falls back to another victim. Counter
`InterlockedAdd`s that raced in before the claim are discarded by the
counter-overwrite — acceptable, bounded, self-healing. (Today's membership-only
Pass 2 tolerated a non-atomic `Store`; persistent counters do not.)

## Unbiasedness

Nothing in the estimator math changes:

- Sampler slot selection stays **uniform** → the per-slot mixture weight is still
  `1/numAccepted`, so `lcEvaluateMixturePdf` is unchanged.
- Eviction and carry change only **which lights occupy a cell**. The sampler and
  the pdf both read the identical Prev cell, so for *any* cell contents the
  estimator is unbiased — exactly the base-plan invariant.
- Stale statistics (carried from prior frames, decayed) affect only *which* seeds
  survive (variance), never the pdf (bias).
- The new counter words are **never read by the sampler** (`lcSelectSubtreeRoot`
  reads only `slots[]` / `normalTags[]`) — no statistic can leak into the
  selection probability. They are read only by eviction (cell membership) and the
  carry decay.

**Inherited invariant to keep watching (audit note).** Unbiasedness still requires
the sampler and the emission-MIS pdf to reproject to the **same** Prev cell. They
key off different surfaces — the NEE surface vs the BSDF-hit surface at the same
pixel — so a sub-bucket/tile mismatch there would mismatch the pdf. This risk is
inherited from the base plan (covered by `test_cache_mis.py`), but **persistence
raises the stakes**: with mostly-empty cells a latent mismatch was masked; with
cells now full of carried content a mismatch surfaces as real divergence. Keep
this case in the MIS test once cells persist.

`test_cache_mis.py` must still pass unchanged, and the `/numAccepted` negative
control must still trip.

## Settings (additions)

| Setting | Default | Range | Description |
|---|---|---|---|
| `rtslCacheStatDecay` | 0.95 | [0.5, 0.99] | EWMA decay per carry (effective window ≈ 1/(1−d) frames ≈ 20) |
| `rtslCacheEvictPriorStrength` | 2.0 | [0, 16] | Eviction prior weight (effective attempts); mean = cell rate, 0.5 when empty |

Both exposed via `SettingsManager` + the (now default-open) RTSL Cache GUI
section.

**These two do NOT wire `didPathTracingSettingsChange` (audit fix).** They change
only the decay/prior *arithmetic*, not cell addressing or acceptance, so they
don't need a `suppressPrev` reset — a changed decay simply applies from the next
carry, a changed prior from the next eviction. Wiring them would empty all cells
and force a ~20-frame re-warm on every slider nudge, making the very knobs you tune
by watching steady-state behavior impossible to tune. (Keep the wiring for
`enabled`, `levels`, `lightsPerCell`, and the depth/normal thresholds, which *do*
change addressing/acceptance.) Note: they still shouldn't reset path-trace
accumulation either, since they don't change the converged image — only its noise.

## Implementation steps

### Step W1 — Slot layout + params plumbing
**Status: DONE (2026-05-23).** See the "W1 done" changelog entry for deltas.
- `RTSL_TILE_CACHE_SLOT_BYTES` 8 → 16; add `RTSL_CACHE_STAT_SCALE` and carry
  workgroup constants.
- Double the RAW UAV/SRV element counts; bump the one-shot init clear to zero the
  two new words.
- `RtslCacheParams`: add `statDecay`, `evictPriorStrength` (+ pad). Upload in
  `renderer.cpp`. Register the two settings + GUI sliders.
- `tile_cache.hlsli`: update `tcSlotBase` stride; add counter load/store helpers.
  Counters written 0, not yet read. No behavior change. Goldens pass.

### Step W2 — Carry pass (replaces clear)
**Status: DONE (2026-05-23).** See the "W2 done" changelog entry for deltas
(carry root sig is GLOBAL_PARAMS + `rtslLightToLeaf` root SRV, not GLOBAL_PARAMS
only; `tile_cache_cells.hlsli` header split; `linearDepthCurr` UAV→NPSR→restore).
- New `rtsl_tile_cache_carry.cs.hlsl` + `GLOBAL_PARAMS`-only root sig + PSO
  (hand-register in `shaders.cpp`).
- `renderer.cpp`: add the `linearDepthCurr` UAV→NPSR transition (restore after);
  swap the per-frame clear dispatch for the carry dispatch (keep `carry → UAV
  barrier → PT`; keep the one-shot init clear for cold start).
- Carries `lightIdx`/`normalTag` (+ decayed counters, all four words) with
  tile-center reproject, disocclusion reject, and the dead-light drop. Cell
  contents now persist instead of clearing.
- **Behavior changes** (persistent membership) but stays unbiased. Validate
  representative goldens (`two_triangles`, `cornell_box_rtsl`, `cave_lights`) and
  `test_cache_mis.py`.

### Step W3 — Outcome attribution
**Status: DONE (2026-05-23).** See the "W3 done" changelog entry for deltas
(`prevSlotBase` out-param + `TileCacheSeed`, `tcInsert`→`tcUpsert` with
CAS-claimed Pass 2, occlusion result threaded out of `sampleDirectLightingRtsl`,
write block moved outside the `didHitLight` guard and gated `#if !NRC_UPDATE`,
signal liveness by construction; empirical dump deferred to W5).
- `lcSelectSubtreeRoot`: add the `out` seed descriptor (X lightIdx + X's decayed
  Prev counters, or sentinel).
- `path_tracing.rgs.hlsl`: **restructure the NEE branch so the occlusion result is
  visible outside the `didHitLight` guard** (audit fix #1); record the X outcome
  (attempt always, success conditional) via `tcUpsert(X, +1, +hit, seedCounters)`
  and the Y vote via `tcUpsert(Y, 0, 0, prior)`; collapse to the X call when X==Y.
  **Gate all counter writes behind `#if !NRC_UPDATE`** (audit fix #3).
- Counters populated, **not yet used** by eviction (Pass 2 still uniform). RNG
  unchanged (seed thread-through draws no extra random). Validate goldens + MIS,
  **plus the signal-liveness check** (validation item 6): confirm rates are not
  all ≈ 1.0 — i.e. failures are actually being counted.

### Step W4 — Weighted eviction + confidence prior
- `tcInsert` / `tcUpsert` Pass 2: replace the uniform random victim with the
  `rate_est` min-scan (cell-relative prior, ties by attempts), CAS-claimed.
- Now counters affect cell contents. Validate goldens + MIS + an occlusion-heavy
  scene (e.g. `cave_lights`) and a **dynamic** scene (moving occluder + camera) —
  expect equal-or-better convergence at fixed sample count, never bias.

### Step W5 — Validation, tuning, docs
- Sweep `statDecay`, `evictPriorStrength`; pick defaults from convergence-vs-time.
- Throughput: carry pass cost vs the old clear; confirm within the base plan's
  0.85× cache-on budget.
- Knowledgebase entry once the feature lands.

## Validation

1. **Unbiasedness held.** `test_cache_mis.py` passes unchanged; `/numAccepted`
   negative control trips. (Estimator math untouched.)
2. **Goldens.** 48-entry suite under threshold with the cache on; byte-identical
   with the cache off (carry/upsert all gated behind `enabled`).
3. **Convergence win.** On an occlusion-heavy scene, cache-on with weighting
   reaches a target RMSE in fewer samples than cache-on without weighting
   (uniform eviction). Pass: weighted ≤ uniform at matched samples.
4. **Carry correctness.** Synthetic test: a moving occluder. Verify disoccluded
   cells reset (no stale lights leak through the depth reject) **and recover**
   within N frames once the edge stabilizes (not just that they reset — guards the
   double-disocclusion cold-band, see Risks).
5. **Throughput.** Carry pass ≤ a small multiple of the clear; total cache-on
   ≥ 0.85× cache-off.
6. **Signal liveness (audit fix #1).** Debug-dump per-slot `(attempts, successes)`
   on an occlusion-heavy scene; confirm a non-trivial fraction of slots have
   `successes < attempts` (failures are counted). Pass: rates are **not** all
   ≈ 1.0. This is the test that catches "recorded only on hits"; without it, a
   dead signal passes every other check.
7. **Dynamic scene.** Moving occluder + camera motion (not just static
   convergence). Confirms the decay/window choice tracks visibility changes and a
   revealed light recovers before being evicted; static RMSE sweeps alone pick a
   decay that's too slow for motion.

## Files

- `common_settings.h` — slot bytes, stat scale, carry workgroup constants.
- `common_params.h` — `RtslCacheParams` additions.
- `common_registers.h` — carry root-sig register space if needed.
- `tile_cache.hlsli` — slot stride + counter helpers (rounded/clamped),
  `lcSelectSubtreeRoot` seed out-param, `tcUpsert` (find-or-insert + atomic add),
  CAS-claimed weighted eviction with cell-relative prior, carry-pass dead-light
  drop helper.
- New `tile_cache/rtsl_tile_cache_carry.cs.hlsl` + registration (GLOBAL_PARAMS-only
  root sig).
- `path_tracing.rgs.hlsl` — restructure the NEE branch so the occlusion result is
  visible outside the `didHitLight` guard; seed thread-through; X/Y `tcUpsert`
  calls gated `#if !NRC_UPDATE`.
- `renderer.cpp` / `renderer_pipeline.cpp` — carry PSO/root sig + dispatch swap,
  the `linearDepthCurr` UAV→NPSR→restore transition, param upload.
- `settings_manager.cpp` / `renderer_gui.cpp` — two new settings + sliders.
- `tests/tests.json` — occlusion-convergence golden(s).

## Risks and open questions

- **Tile-center carry coarseness.** One reprojection + one disocclusion test per
  tile (8×8 px), applied to all four sub-buckets — wrong for silhouette tiles. Not
  a correctness issue (read-path reject still guards the sampler; worst case is
  variance). Planned fix is the per-sub-bucket in-tile probe (see Carry pass),
  deferred to after W4 so it can be measured.
- **Seed-source vs carry-source mismatch (resolved, with a cost).** The per-pixel
  seed cell differs from the tile-center carry source at tile boundaries. Resolved
  by upserting X **with eviction** into the insert cell (audit fix #5) so the
  outcome always lands — but this adds inserts/evictions (more churn) and means the
  per-pixel X-insert and the tile-center carry both populate cells (dedup by
  lightIdx prevents duplicates). Watch carry-pass + insert churn in W5 throughput.
- **Counter precision.** Fixed-point scale + decay; rounding (not floor) keeps the
  effective decay honest; clamp at store. NVAPI float atomics are the escape hatch.
- **Decay tuning, esp. voxel mode.** Too slow → stale visibility lags moving
  occluders and a revealed light may be evicted before its rate recovers; too fast
  → noisy rates. **Voxel mode is the hard case:** chunk load/unload churns light
  topology almost every frame, so a 20-frame EWMA may never settle. The default
  0.95 is tuned for static/slow scenes; the W5 sweep must include a dynamic scene
  and may pick a faster decay (lower default) for voxel mode.
- **Glass / passthrough (Option B).** A glass primary hit keys the tile to the
  glass surface; under persistence it now *accumulates* a success history for
  lights seen through glass, which is geometrically unstable. Worse than neutral
  but rare. If W5 flags glass-tile anomalies, suppress insert+outcome there via the
  Option-A `PAYLOAD_FLAG_FIRST_HIT_WAS_PASSTHROUGH` flag.
- **Carry copy is not coalesced.** One-thread-per-cell copies K×16 B serially with
  per-cell strides — chosen for the per-cell reproject decision, not bandwidth. Fine
  for a once-per-frame pass; if W5 shows it over budget, the lever is
  one-thread-per-slot with the cell deciding the reproject source once in
  groupshared, then lanes copying coalesced. Do **not** assume parity with the
  clear's grid-stride coalescing.
- **Attribution granularity.** The score reflects X's `L`-ancestor subtree, not X
  alone; slots sharing a subtree root converge to the same rate (acceptable, even
  useful for dedup-style eviction). Eviction ties among them break by attempts.

## Out of scope (follow-ups)

- **Weighted slot selection** (the variance win that *does* touch the pdf —
  per-slot mixture weight ∝ rate, mirrored in `lcEvaluateMixturePdf`, hoist
  becomes a weighted sum). Natural next step once weighted eviction is validated.
- Per-sub-bucket or per-pixel scatter-carry.
- Combining learned visibility with light power for the eviction/selection score.
