_Last edited: 2026-09-17_

# Startup

`Renderer::init()` runs after the window is created, so its whole duration is the white window
the user sees. Three fixed costs dominate and none of them is in our code, so init is arranged
to overlap them rather than shrink them (measured 2026-09 on an RTX 4070 SUPER):

- **`slInit`** (~0.6 s): loading the Streamline plugin DLLs. `initStreamline()` runs it on a
  worker thread; nothing before device creation needs it.
- **`slSetD3DDevice`** (~0.8 s): NGX initialization for DLSS-RR / DLSS-G on the device.
- **First `CreateStateObject`** (~1.1 s): a one-off DXR initialization inside the driver, paid
  by whichever RT pipeline is created first. With a cold driver shader cache each pipeline
  additionally costs ~1 s of compile.

## Manual hooking rationale

The app uses Streamline's manual-hooking mode and creates the DXGI factory and D3D12 device
natively (entry points fetched from `d3d12.dll` / `dxgi.dll` by hand, because `sl.interposer`
exports the same names and is linked first). The native device exists ~0.15 s into init, before
`slInit` has finished, which is what lets `startRtPipelineCreation()` kick off the RT pipelines
that early. Once `slInit` is joined, `slUpgradeInterface` wraps the native device and factory in
proxies; only the calls Streamline hooks (command queue creation, swap chain creation, present)
go through the proxies. Everything else, including all pipeline creation, uses the native
interfaces, as the manual-hooking guide requires.

Gotchas:

- `slUpgradeInterface` returns an *owned* reference to a new proxy that has already AddRef'd the
  native object, so it must be `Attach`ed, not `QueryInterface`d into a `ComPtr` (that leaks the
  proxy and keeps the device alive past `destroy()`). When the interposer is disabled it leaves
  the pointer unchanged and adds no reference, which `upgradeToSlProxy` handles.
- The Streamline headers say `slUpgradeInterface` / `slSetD3DDevice` should be called
  "immediately after" the base interface is created and that `slInit` must precede any D3D call.
  Deliberately ignored: the manual-hooking guide allows native creation before `slInit`, and the
  upgrade is a pure wrapper with no dependence on what was done through the native interface
  before it.
- `slFeatures` lives at file scope because the `slInit` thread reads `featuresToLoad` after
  `initStreamline()` has returned.

## RT pipeline worker

`startRtPipelineCreation()` runs before the `slInit` join to get the overlap, and its worker
sets up its own prerequisites (NVAPI extension slot, root signatures, `sharcInit`) so no main
thread step has to be ordered ahead of it. The four RT pipelines are built on separate threads
because the driver compiles them concurrently on a cold cache (~2 s in parallel instead of ~4 s
in sequence). `initPipeline()` joins the worker and then builds the cheap compute/graphics PSOs.

Anything on the main thread between `startRtPipelineCreation()` and `initPipeline()` must not
read the RT PSOs, dispatch descs, root signatures, `renderState.useSer` or
`renderState.sharc.supported`.

The NVAPI extension slot has to be set with the device-global
`NvAPI_D3D12_SetNvShaderExtnSlotSpace`, not the `LocalThread` variant: the state objects are
created on the per-pipeline threads, not on the thread that set the slot.

## Over-the-air updates

Streamline's OTA update check is on by default and costs ~0.4 s of network round trip inside
`slInit`, so `initStreamline()` clears `eAllowOTA` / `eLoadDownloadedPlugins`. The trade-off is
that new plugin versions only arrive with an SDK update in `external/streamline`.

## Timing log

`timedInitStep()` wraps the steps above so a slow launch can be attributed from the log without
re-instrumenting.
