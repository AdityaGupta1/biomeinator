# TODO

## ReSTIR: light index translation across frames

When the player crosses a chunk border (or any chunk remesh happens), the area lights
buffer gets rebuilt and light indices can be reordered. Reservoirs stored from the
previous frame (`risSamplesPrev`) still hold old `lightIdx` values, so temporal reuse
re-evaluates and shades the *wrong* lights for a frame — visible as a flash of darkness
across the whole screen as the samples go crazy.

Fix (RTXDI does this via `RAB_TranslateLightIndex`):
- Maintain a persistent light ID per area light that survives remesh (e.g. hash of
  instance + triangle, or an explicit stable ID assigned at light creation).
- Each frame, build a GPU mapping table from previous frame's light index -> current
  frame's light index (or invalid if the light disappeared).
- In temporal reuse, translate `reproj_risSample.lightIdx` through this table before
  use; kill the reservoir (W = 0, keep confidence) if the light no longer exists.

Reference: `..\RTXDI\Libraries\Rtxdi\Include\Rtxdi\DI\TemporalResampling.hlsli` lines
125-148 (translation + reservoir kill on missing light).
