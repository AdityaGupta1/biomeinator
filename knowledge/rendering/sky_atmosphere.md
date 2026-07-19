_Last edited: 2026-07-19_

# Sky Atmosphere (PBR sky, stage 1)

Physically based sky per Hillaire's EGSR 2020 technique (see `plans/pbr-sky.md` for the full
plan and paper references). `src/rendering/sky_atmosphere.cpp` owns two LUT textures and their
compute passes; `shaders/sky/atmosphere.hlsli` holds the shared constants and
parameterizations; `shaders/light/dome_light.hlsli` consumes both LUTs.

## Ordering invariants

- The transmittance LUT depends only on atmosphere constants, so it is generated once on the
  first `dispatch()` call; the sky-view LUT is regenerated every frame (sun moves with
  `animTime`, and it tracks camera altitude). The per-frame dispatch must complete (UAV
  barrier) before the path trace pass samples the LUTs.
- `SkyAtmosphere::init()` must run before `initRtTargets()` — the first `resize()` records the
  LUT SRV indices into every frame's `heapIndices`.
- The dispatch is bindless (root constants carry heap indices), so it sits after the
  `SetDescriptorHeaps` call in the render loop, inside the TLAS-gated block. It is skipped
  entirely when not in voxel mode (the dome light is black there anyway).

## Units and calibration

- The sky-view LUT stores luminance for **unit sun illuminance**; `dome_light.hlsli` multiplies
  by `sunIlluminance` at the lookup. This keeps calibration, and later a moon (a second
  directional light reusing the same LUT machinery), out of the LUT.
- `sunIlluminance` (~10 lux) is calibrated to match the *total power* of the old hand-tuned sun:
  old radiance 16000 × the oversized disk's solid angle (`2π(1 − 0.9999)`). The disk radiance is
  `illuminance × transmittance / solidAngle`, so noon exposure is unchanged while sunset
  reddening/dimming comes from the transmittance fetch for free.
- `nightAmbient` is a constant floor added at the lookup (not baked into the LUT) so nights
  aren't pitch black; delete it when the moon lands.

## Gotchas

- The Bruneton-Neyret transmittance parameterization only covers rays that miss the ground
  sphere. Two consequences handled in `dome_light.hlsli`: the sun disk must be explicitly
  zeroed when the ray toward it intersects the virtual planet (`isSunOccluded`), and occluded
  disk directions fall through to the sky-view LUT (which contains the virtual planet's ground
  below the horizon) instead of returning black — otherwise a half-black disk straddles the
  horizon at sunset.
- The sky-view uv↔direction mapping (sun-relative longitude, quadratic latitude) must match
  exactly between generation and lookup — both sides call the shared helpers in
  `atmosphere.hlsli`; don't reimplement either half.
- `RT_REGISTER_LUT_SAMPLER` exists because the RT `texSampler` is point-filtered in voxel mode;
  the LUTs need bilinear + clamp.
- Sky changes invalidate goldens that see the sky (`water_absorption`, `water_reflection`,
  `underwater`); rebaseline after each stage of the sky plan, not between sub-steps.

## Not yet done (stage 2)

Multiple scattering (paper §5.5). Without it, twilight is near-black and the sunset zenith is
darker than reality; the stage 2 multi-scattering LUT slots between the transmittance and
sky-view passes at startup.
