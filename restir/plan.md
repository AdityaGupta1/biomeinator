**Key difference from old implementation:** RTSL (light tree traversal) replaces RIS for initial candidate generation. RTSL already selects a light + point on light — store that as the reservoir sample instead of running RIS candidates.

## 1. ReSTIR sample struct + buffers [DONE]
- Expand `RisSample` (or create a new `RestirSample` / `Reservoir`) with `p_hat`, `confidence`, `pad` fields — old struct had these, current `RisSample` dropped them
- Triple-buffer allocation in `RendererState` (3 `ComPtr<ID3D12Resource>`) with in/out/prev pointers + `swapRisBuffers()` / `storePrevRisBuffer()`

## 2. GBuffer pass — initial candidate generation via RTSL [DONE]
- Instead of old RIS candidate generation (multiple light+BSDF candidates → weighted reservoir), call `selectLightFromSubtree` + `sampleAreaLightPoint` to pick one light+point
- Compute `p_hat` via `risTargetFunction` (or an RTSL-aware version)
- Set `W = (p_hat > 0 && q > 0) ? 1/q : 0` where `q = pdfSelect * lightSamplePdf` (single-sample reservoir, confidence = 1)
- Optional visibility check: trace shadow ray, zero W if occluded
- Write to `risSamplesOut` buffer
- GBuffer needs RTSL light tree SRV bindings (currently only PT has them)

## 3. Temporal reuse compute shader (new `temporal_reuse.cs.hlsl`)
- Largely same as old — reproject via motion vectors, depth/normal comparison, pairwise MIS, geometry term Jacobian, confidence-weighted combination
- Reads `risSamplesIn` + `risSamplesPrev`, writes `risSamplesOut`
- Needs its own root signature, PSO, register space, workgroup size constants

## 4. Spatial reuse compute shader (new `spatial_reuse.cs.hlsl`)
- Same as old — random spatial neighbors, normal/depth rejection, pairwise MIS with Jacobian
- Own root signature, PSO, register space

## 5. Path tracing integration
- Add `SamplingMode::RESTIR` enum value
- At depth 0: read final reservoir from buffer, call `evaluateRisSample` for shadow ray + contribution
- At depth > 0: fall back to RTSL inline (same as current RTSL mode)
- MIS weights: use RTSL selection pdf × area-sampling pdf (via `evaluateLightSelectPdf` which already exists). BSDF-hit MIS at depth 0 uses `lightPdfRtsl`.

## 6. Renderer plumbing
- Register spaces, root signatures, PSOs for temporal + spatial reuse
- Buffer creation/destruction in resize/destroy
- Dispatch temporal reuse (skip frame 0), then spatial reuse, between gbuffer and PT
- `restirDoVisibilityCheck` setting + GUI checkbox
- `prevDepthAndNormal` target transitions for temporal reuse

## Post-implementation
- Revisit ReSTIR tests: add more scene coverage beyond `cornell_box_restir` (e.g. fancy_cornell_box, evil_room, cave, glass scenes) with `--samplingMode=4`
