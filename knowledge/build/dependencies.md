_Last edited: 2026-09-06_

# Third-Party Dependencies

Everything compiled or linked lives under `external/`. Read-only documentation lives under
the top-level `reference/` instead (see [reference/](../reference/index.md)); keep the two
apart so that nothing in `reference/` can accidentally become a build input.

## Three kinds of dependency

- **Git submodules** (`.gitmodules`) for source that is built in-tree or used header-only:
  imgui, implot, FastNoise2, lz4, GPUSorting, nvapi, DirectX-Headers. Update by moving the
  submodule commit.
- **Vendored prebuilt SDKs**, committed as files: AgilitySDK, streamline, dxc,
  WinPixEventRuntime. Each folder carries its own `LICENSE.txt` at the root and splits into
  `include/`, `lib/`, `bin/` (or the SDK's native layout when it ships one, as streamline
  does). These are committed rather than fetched so the DLLs always match the headers
  exactly.
- **`external/_licenses/`** for things that arrive without their own folder: FetchContent
  packages (glm) and code adapted piecemeal from other projects (Cycles, Khronos PBR
  Neutral).

## Adding a prebuilt SDK

Four touchpoints in `CMakeLists.txt`, all on the `Biomeinator` target, plus the license file
in the SDK folder:

1. include directory in `target_include_directories`
2. library directory in `target_link_directories`
3. library name in `target_link_libraries`
4. DLL path in `RUNTIME_DLLS`, so the post-build copy places it beside the exe

## WinPixEventRuntime

Sourced from the NuGet package, which is a plain zip: download
`https://www.nuget.org/api/v2/package/WinPixEventRuntime/<version>` and unpack it, no NuGet
client needed. Only the x64 non-UAP `.lib`/`.dll` and the headers are vendored. MIT licensed.

- **`pix3.h` must be included after `windows.h` is in scope.** It uses `PCWSTR` and friends
  without including them itself, so putting it at the top of a file fails with a wall of
  unknown-type errors. In this codebase that means after the DXR and Streamline includes.
- Marker availability per config is decided in `CMakeLists.txt`; see
  [configs.md](configs.md). Marker calls are always safe to write because the header turns
  them into no-ops when `USE_PIX` is undefined.
