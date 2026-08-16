_Last edited: 2026-08-16_

# Nsight Aftermath GPU crash dump instrumentation

Recorded as a reference diff rather than live code:
[aftermath_instrumentation.patch](aftermath_instrumentation.patch). It touches 14 files
across the buffer, light-tree, sort and renderer layers, which is far too invasive to carry
permanently for something only needed during an investigation — but it is also too laborious
to work out from scratch a second time, and it is what root-caused the
`areaLightSamplingStructure` race (see [gpu → mapped_array.md](../gpu/mapped_array.md)).

**Read it, don't replay it.** It is a snapshot against the state of the tree in August 2026
and will drift out of applicability as the code moves. Use it to see which files need
touching and what goes in each one, then reimplement against whatever the tree looks like at
the time. The parts worth copying are the whole of `aftermath.h`/`aftermath.cpp` and the
CMake SDK-discovery block; the rest is one-line call sites.

## What it adds

Everything is behind `--aftermath` and compiles out entirely when CMake cannot find the SDK.

- **Crash dumps** — `.nv-gpudmp` plus the `.nvdbg` shader debug info, written from the
  `GFSDK_Aftermath` callbacks into `Documents/biomeinator/gpu_crash_dumps/`.
- **Pass markers** — `Aftermath::setMarker` at pass boundaries (`scene update`, `lt_clear`,
  `lt_collect`, the `sort_*` passes, `gbuffer`, `path tracing`), so a dump says which pass
  the queue had reached.
- **Resource VA tracking** — every buffer's GPU VA range and debug name, dumped alongside
  each crash as a sibling `.buffers.txt`.

## Why over DRED

DRED auto-breadcrumbs record which *op index* last completed, but completion means "command
consumed", not "shader finished" — so a breadcrumb cannot separate a hung dispatch from one
whose successor barrier is waiting on unrelated work. DRED also reported a page-fault VA of
`0` on every capture here, which is what made an earlier investigation conclude the GPU was
hanging when it was actually faulting. Aftermath resolves the faulting warp and the access
type directly.

## Gotchas

- **Mutually exclusive with `--gpuValidation`.** Aftermath returns
  `FAIL_D3DDebugLayerNotCompatible` when the D3D12 debug layer is on, so `enableCrashDumps`
  exits rather than silently producing nothing.
- **`enableCrashDumps` must precede device creation**, hence its position at the top of
  `initDevice`, ahead of the Streamline-interposed `D3D12CreateDevice`. Aftermath is
  initialized with the *native* device, not the Streamline proxy.
- **The 64-bit "Shader hash" in a dump is not the DXIL container digest.** Comparing it
  against the digests dxc embeds in the generated `.fxh` blobs never matches, and concluding
  from that that a shader is driver-internal is wrong.
- **Nsight needs standalone shader binaries, which the build never writes.** dxc emits C
  arrays via `/Fh` that get compiled into the exe, so "shader binary file not found" is
  expected until you extract the blobs out of `build/generated_shaders/*.fxh` into `.cso`
  files and point the *shader binary search path* at them. That is separate from the
  *shader debug info* path, which is the crash dump directory.
- **Resource registrations are deliberately never released.** Unregistering a destroyed
  resource would discard exactly the tracking data that identifies a use-after-free, so the
  handles leak for the life of the process — acceptable only because this is debug-only.
- **`waitForCrashDump` polls for 5 s.** The fence timeout is not synchronized with crash
  detection; in practice the driver often finishes the dump a second before the fence gives
  up, but nothing guarantees that ordering and returning early loses the dump.

## Reading a dump

GPU VA is recycled aggressively once a resource dies, so several stale `.buffers.txt`
records can cover one fault address. **The record with the highest `created` ordinal is what
the address meant at fault time** — the older ones are previous tenants.

`nv-aftermath-format.exe` (under the Nsight Graphics host directory) decodes a dump to text
and is scriptable for stress loops. The GUI is needed for shader source resolution.

## Reproducing a GPU fault

The golden test runner builds its command line from `tests.json` and cannot inject extra
flags, so stress runs have to launch `Biomeinator.exe` directly with the same arguments the
runner would use, plus `--aftermath=true`, and grep stderr for `fence wait timed out`.

`cave_lights`, `underwater`, `water_absorption` and `fog_god_rays` reproduced most often.

**Crash rate is a poor signal for evaluating a fix.** It moves with GPU load — an unrelated
game running in the background roughly doubled it — and 40 launches cannot resolve a 2×
change. Prefer a structural argument, and use the rate only as confirmation.
