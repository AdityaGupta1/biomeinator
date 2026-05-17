# RTSL Cleanup / Follow-ups

Audit findings from the post-Stage-4 code review and refactor. Items marked
✅ landed; remaining items are open follow-ups ordered by priority.

---

## ✅ 1. Drop the BRDF bound from the HIS weights — DONE (2026-05-17)

**File:** `src/shaders/common/light_tree_sampling.hlsli` (`rtslChildProbs`,
`selectLightFromSubtree`, `evaluateLightSelectPdf`, `sampleDirectLightingRtsl`,
`lightPdfRtsl`).
**Also affected:** `src/shaders/path_tracing/path_tracing.rgs.hlsl`
(`prevBrdfBound` / `prevAllowPruning` stash deleted).

The HIS weights were `w = F · I · G / d²` with `F = luminance(brdfBound)`.
The reduction to a scalar luminance meant F was the **same factor for both
children**, so it cancelled in the ratio `wMin1 / (wMin1 + wMin2)`. F therefore
contributed nothing to per-child selection — its only role was gating the
dead-branch fallback when F was zero. Glossy-only materials hit this
fallback (no diffuse → F = 0 → all `core = 0` → dead-branch behavior).

The reference impl
(`RealTimeStochasticLightcuts/Shaders/LightTreeUtilities.hlsli` ::
`normalizedWeights`, used by `firstChildWeight` in `SLCHelperFunctions.hlsli`)
also omits F. It uses pure `I · G`, identical to what biomeinator's code did
after the `luminance` factor cancelled.

**Change landed:** weights are now `core = G · flux`; signatures dropped
`brdfBound` and `allowPruning`; the `rtslDiffuseBrdfBound` / `rtslAllowPruning`
helpers and the `prevBrdfBound` / `prevAllowPruning` stash were deleted; the
dead-branch test became purely geometric (`flux == 0 || geomTermBound == 0`),
universally valid for any BSDF since a back-facing bbox cannot produce a light
path regardless of surface response.

**Tradeoff:** lost diffuse-albedo importance weighting in Lambertian descent —
acceptable since the cancellation made it a no-op anyway. Strict improvement
for glossy-only surfaces, which now get geometric importance sampling instead
of degenerating.

---

## 2. Stage 5 hazard — `evaluateLightSelectPdf` must accept `subtreeRoot`

**File:** `src/shaders/common/light_tree_sampling.hlsli`.

`selectLightFromSubtree` already accepts `subtreeRoot` (currently always called
with `0u`), but `evaluateLightSelectPdf` hardcodes `cur = 0u`. The two sides
agree today only because both walk from the root.

When Stage 5 (cut sharing) lands, the forward sampler will descend from a
non-root subtree. The MIS recovery walk **must** start from the same subtree
or `pdfSelect` will silently disagree — a bias bug that no test will catch
before goldens drift.

**Action:** when implementing Stage 5, extend both signatures in lockstep.
Add a unit test that for fixed `(x, normal)` runs the forward sampler N times
and compares the empirical leaf histogram to per-leaf `evaluateLightSelectPdf`
output — χ² should match.

---

## 3. `firstbithigh(treeLeafCount)` assumes `M` is a power of two

**File:** `src/shaders/common/light_tree_sampling.hlsli`
(`evaluateLightSelectPdf`).

`firstbithigh(M)` returns the MSB index, which equals `log2(M)` only when M is
a power of two. The light tree builder enforces this
(`nextPow2AtLeast(LIGHT_TREE_LEAF_FLOOR, ...)`), so the invariant holds today.

If a future change ever relaxes the pow2 constraint on M, this silently
miscomputes `logM` → wrong path length → wrong leaf → silent bias.

**Action:** either add `Util::assert(isPow2(M))` in the C++ builder near the
`nextPow2AtLeast` call (already implicit, but worth a comment), or replace
the `firstbithigh` call with a `bitCount(treeLeafCount - 1u)` form that
also works for non-pow2. Low priority — defensive only.

---

## ✅ 4. Dead-code guards in HIS rescale — DONE

The `(p > 0.0f) ? r/p : r` guards in `selectLightFromSubtree` were dead.
Replaced with unconditional `r /= p1` / `r = (r - p1) / p2`, with a comment
explaining why the divides are safe (`p1 + p2 == 1` always, dead-branch case
caught above).

---

## ✅ 5. `pdfSelect > 0.f` check is redundant — DONE

The defensive check at the old `path_tracing.rgs.hlsl:297` was dropped during
the `sampleDirectLightingRtsl` extraction. `selectLightFromSubtree` only
returns `true` after a complete descent where every chosen `pi > 0`, so
`gotLight == true` already implies `pdfSelect > 0`.

---

## 6. `1e-20f` epsilon vs reference's algebraic cancellation

**File:** `src/shaders/common/light_tree_sampling.hlsli` (`rtslChildProbs`).

```hlsl
const float wMin1 = dropDistanceFromMin ? core1 : (core1 / max(dMinSq1, 1e-20f));
const float wMin2 = dropDistanceFromMin ? core2 : (core2 / max(dMinSq2, 1e-20f));
```

The reference impl avoids this epsilon by computing the ratio symbolically via
`normalizedWeights(l2_min0, l2_min1, intensGeom0, intensGeom1) = l2_min1 * intensGeom0 / (l2_min0 * intensGeom1 + l2_min1 * intensGeom0)`,
which is well-defined when one (but not both) `l2_min` is zero.

Equivalent in result, but biomeinator's eps-floored form can produce `1e20`-magnitude
intermediates. **No bias** (both forward and recovery use the same arithmetic).
Worth replacing for numerical hygiene if the file is touched again.

---

## 7. Tiny non-zero `pdfSelect` slipping past `selectLightFromSubtree`

**File:** `src/shaders/common/light_tree_sampling.hlsli`
(`selectLightFromSubtree`), consumed in `sampleDirectLightingRtsl`.

`selectLightFromSubtree` only rejects branches where `p1 + p2 == 0` exactly;
FP accumulation across a deep descent can yield `pdfSelect = ε` (very small
but >0). Caller divides by `pdfOrW_Y = pdfSelect * lightSamplePdf` in the MIS
branch, producing fireflies. Same risk exists in `evaluateLightSelectPdf` on
the MIS-recovery side — they stay numerically consistent (same FP pattern at
the same shading point), so it is **unbiased**, just noisy.

**Action:** if firefly artifacts surface in tests, add a backstop clamp like
`if (pdfSelect < 1e-10f) return false;` in `selectLightFromSubtree` AND the
matching check in `evaluateLightSelectPdf` to keep both sides consistent.
Pre-existing risk, not introduced by the refactor.

---

## 8. Header guard inconsistency (informational)

`light_tree_sampling.hlsli` and `light_tree.hlsli` use `#ifndef`; everything
else in `src/shaders/` uses `#pragma once`. Pick one. If `#pragma once` works
for the light-tree headers (try the build), unify on that. The leading comment
in `light_tree_sampling.hlsli:12-14` about DXC `#pragma once` reliability is
misleading — other headers reaching the same `util/*` transitively use
`#pragma once` without issue.

---

## ✅ 9. Vestigial defensive ternaries in `rtslChildProbs` — DONE

The `(sumMin > 0) ? wMin1/sumMin : 0` and `(sumMax > 0) ? wMax1/sumMax : 0`
ternaries were unreachable in the non-dead-branch path (both sums are
strictly positive once the early-out at line 149 is skipped). Simplified to
unconditional divides with a comment explaining the invariant.

---

## Suggested order of attack (remaining)

1. Item #2 (subtree-root parameter) — defer until Stage 5 starts.
2. Item #3 (pow2 assertion) — defer; defensive only.
3. Item #6 (`1e-20f` cleanup) — opportunistic.
4. Item #7 (firefly clamp) — wait for evidence in test goldens.
5. Item #8 (header guards) — low value, low effort.
