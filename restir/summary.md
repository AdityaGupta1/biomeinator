## Enum / settings
- `SamplingMode::RESTIR` enum value (was mode 3)
- `restirDoVisibilityCheck` param in `RenderParams` + CLI option + ImGui checkbox
- `RESTIR_MAX_CONFIDENCE` constant (was 8)

## RIS sample buffer infrastructure
- **3 RIS sample buffers** (`dev_risSamples1/2/3`) with in/out/prev pointers — triple-buffered for temporal reuse
- `swapRisBuffers()` — swapped in↔out and did state transitions
- `storePrevRisBuffer()` — saved current frame's input as prev for next frame's temporal reuse

## GBuffer pass — RIS sample generation
- GBuffer wrote `RisSample` per pixel when mode was RIS or RESTIR
- For RESTIR with visibility check enabled, traced shadow ray at sample generation time and zeroed W on miss (biased but better reuse)

## Temporal reuse compute shader (`temporal_reuse.cs.hlsl`, deleted)
- Reprojected using motion vectors + depth/normal comparison (4×4 neighborhood search)
- Pairwise MIS between current and reprojected sample
- Geometry term Jacobian for domain shift
- Confidence-weighted combination

## Spatial reuse compute shader (`spatial_reuse.cs.hlsl`, deleted)
- 5 random spatial neighbors within 8px radius
- Normal/depth similarity rejection (dot > 0.95, dist < 0.4)
- Pairwise MIS with geometry term Jacobian
- Confidence accumulation capped at `RESTIR_MAX_CONFIDENCE`

## Path tracing changes
- Depth 0 used to read RIS sample from buffer (ReSTIR output); depth > 0 generated inline. Removal made all depths generate inline.
- Light PDF special-casing: ReSTIR DI at depth 0 used per-light PDF (not divided by numAreaLights), with matching adjustment on BSDF sample MIS weight. Removed.

## Shared utility (`restir.hlsli` → `ris.hlsli`)
- `calcGeomTermJacobian()` removed (was used by both reuse passes)
- File renamed, kept RIS target function and sample generation

## Root signatures / PSOs / registers
- Temporal and spatial reuse each had own register space, root signature, compute PSO
- GBuffer had `RIS_SAMPLES_OUT` UAV register; PT had `RIS_SAMPLES_IN` SRV register
- All removed from `common_registers.h`, `common_settings.h`, `renderer.cpp`

## Tests
- 4 golden tests removed (cave, cornell_box, evil_room, many_faces_emitter with `--samplingMode=3`)
