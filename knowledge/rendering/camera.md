_Last edited: 2026-04-26_

# Camera

`src/rendering/camera.h/cpp`. Uses the same int+float split-position system as instances
(see [scene → transform_system.md](../scene/transform_system.md)).

## Split Position

`posInt_WS` (ivec3) + `posFloat_WS` (vec3, always in [0,1) per component). `processInput()`
accumulates movement into `posFloat_WS`, then floors any overflow into `posInt_WS`. The
shader-facing `params.pos_WS` is computed relative to `globalInstanceOffset` so it stays
near zero.

## worldToPrevClipMat Correction

When `globalInstanceOffset` changes between frames, `setMatrices()` applies a translation
correction to `worldToPrevClipMat` so motion vectors stay correct. Without this, every pixel
would show massive motion whenever the camera crosses an integer boundary. The same
correction is applied to `worldToPrevView` for Streamline, though the code notes uncertainty
about whether Streamline actually needs it.

## Jitter

Halton sequence jitter is used for accumulation mode and DLSS. Sequence length is set
externally via `setJitterHaltonSequenceLength()` — DLSS determines the optimal length.
