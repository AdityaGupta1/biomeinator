_Last edited: 2026-09-17_

# Startup

`Renderer::init()` runs after the window is created, so its whole duration is the white window
the user sees. Three fixed costs dominate and none of them is in our code, so init is arranged
to overlap them rather than shrink them:

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

## RT pipeline worker

`startRtPipelineCreation()` must run before `slSetD3DDevice` to get the overlap, and its worker
sets up its own prerequisites (NVAPI extension slot, root signatures, `sharcInit`) so no main
thread step has to be ordered ahead of it. The four RT pipelines are built on separate threads
because the driver compiles them concurrently on a cold cache (~2 s in parallel instead of ~4 s
in sequence). `initPipeline()` joins the worker and then builds the cheap compute/graphics PSOs.

Anything on the main thread between `startRtPipelineCreation()` and `initPipeline()` must not
read the RT PSOs, dispatch descs, root signatures, `renderState.useSer` or
`renderState.sharc.supported`.

## Over-the-air updates

Streamline's OTA update check is on by default and costs ~0.4 s of network round trip inside
`slInit`, so `initStreamline()` clears `eAllowOTA` / `eLoadDownloadedPlugins`. The trade-off is
that new plugin versions only arrive with an SDK update in `external/streamline`.

## Timing log

`timedInitStep()` wraps the steps above and logs `init: <step> took N ms` plus a total, so a
slow launch can be attributed without re-instrumenting. Lines from the worker threads are
assembled before being written (see `logger.cpp`) so they do not interleave.
