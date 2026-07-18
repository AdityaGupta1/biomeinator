_Last edited: 2026-07-17_

# ReSTIR cross-frame reuse invariants

Temporal reuse (`src/shaders/path_tracing/temporal_reuse.cs.hlsl`) carries
reservoirs across frames, which breaks two implicit assumptions that same-frame
passes (initial RIS, spatial reuse) get for free. Both caused a screen-wide
dark flash when the player crossed a chunk border.

## Light indices are only stable until a remesh

A reservoir's `lightIdx` is a sparse index into `managedAreaLightsBuffer`.
Sections are per-instance and free-listed, so indices survive frames *except*
when a chunk remesh frees/reallocates that instance's range — the slot a stale
reservoir points at may then hold a different light or dead data.

Instead of RTXDI's explicit prev→current translation table
(`RAB_TranslateLightIndex`), validation uses data that already exists
(`isReprojectedLightValid`):

- **Round-trip check**: each `AreaLight` slot stores its own
  `(instanceId, triangleIdx)`; requiring that pair to map back to the same
  sparse index through current-frame `instanceDatas` + `perTriDatas` catches
  deleted instances and recycled instance ids.
- **Point-on-triangle check**: the reservoir's `pointOnLight_WS` must still lie
  on the slot's triangle. This catches the case the round-trip cannot: a slot
  reallocated to a *different* light, which validates its own index.

Neither check alone is sufficient; the pair covers all stale-slot cases.

On failure the reservoir is killed RTXDI-style: resampling weight goes to 0
(`reproj_p_hat_this = 0`) but its confidence still counts in the MIS weights,
so the surviving current-frame sample isn't overweighted for a frame.

Supporting invariants outside the shader:

- `Scene::freeInstance` writes `areaLightsBufferOffset = LIGHT_IDX_INVALID`
  into the freed instance's `InstanceData` so the round-trip fails
  deterministically instead of reading stale offsets.
- Instances built with no area lights get the same sentinel — the field would
  otherwise be uninitialized, and validation reads it for recycled ids.

## Prev-frame positions are in the previous frame's render space

All world-space positions on the GPU are render-space (world minus
`globalInstanceOffset`), and the offset changes exactly on chunk-border
crossings — the same event that remeshes chunks. Anything read from prev-frame
data (`risSamplesPrev.pointOnLight_WS`, surface positions reconstructed from
`prevPos_WS` + prev depth) must be translated by
`prevGlobalInstanceOffset - globalInstanceOffset` before mixing with
current-frame positions. Without this, the reprojection position match fails
screen-wide for one frame and reused light points shift by the offset delta.
Motion vectors already had this correction baked into `worldToPrevClipMat`
([camera.md](camera.md)); the reuse shader needed its own.

The translation happens once at ingest, so `risSamplesPrev` is always exactly
one space-delta behind — reservoirs surviving many frames don't accumulate
error.

Related: [light_tree.md](light_tree.md) for the sparse area-light buffer
layout the round-trip check relies on.
