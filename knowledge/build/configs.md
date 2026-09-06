_Last edited: 2026-09-06_

# Build Configurations

**Prefer `RelWithDebInfo` for everyday work, including profiling.** It is the only
configuration that is both optimized and fully instrumented:

```
cmake --build build --config RelWithDebInfo --target Biomeinator
```

Output goes to `build/<Config>/`. `BiomeinatorTests` is a separate target and depends on
`Biomeinator`.

| | Debug | RelWithDebInfo | Release |
|---|---|---|---|
| Optimized | no | yes | yes |
| Symbols | yes | yes | no |
| `ASSERT` (`ENABLE_ASSERTS`) | on | on | off |
| PIX markers (`USE_PIX`) | on (header default) | on (explicit) | off |
| `d3d12SDKLayers.dll` copied | yes | yes | no |

Rationale:

- **Debug is too slow for a path tracer** to be useful beyond stepping through CPU code;
  the debug layer and unoptimized shaders-side host code make frames take long enough that
  timing-dependent behaviour (chunk streaming, fence waits) stops resembling the real thing.
- **Release is deliberately bare.** No asserts and no markers means it measures what a user
  would run, so it is the config to build when checking that instrumentation itself has no
  cost. It is not the config to profile *in*, because Nsight and PIX captures of it have no
  pass names.
- **`USE_PIX` is defined only for RelWithDebInfo** in `CMakeLists.txt`. `pix3.h` enables
  itself in Debug via `_DEBUG`, so that config needs nothing, and Release must stay
  marker-free. Do not define it globally.

Gotchas:

- Source files are globbed, so **adding a file needs a reconfigure** (`cmake -S . -B build`),
  not just a build.
- The three configs share one build tree; only the `build/<Config>/` output directories
  differ. Runtime DLLs are copied per config, so a freshly built config always has its own
  copies.
