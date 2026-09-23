_Last edited: 2026-09-22_

# Build Configurations

**Prefer `RelWithDebInfo` for everyday work, including profiling.** It is the only
configuration that is both optimized and fully instrumented:

```
cmake --build build --config RelWithDebInfo --target Biomeinator
```

Output goes to `build/<Config>/`. `BiomeinatorTests` is the golden-image target and depends on
`Biomeinator`; `BiomeinatorUnitTests` is the independent CPU target registered with CTest. See
[tests → unit_tests.md](../tests/unit_tests.md) for its build and run commands.

| | Debug | RelWithDebInfo | Release |
|---|---|---|---|
| Optimized | no | yes | yes |
| Symbols | yes | yes | no |
| `ASSERT` (`ENABLE_ASSERTS`) | on | on | off |
| PIX markers (`USE_PIX`) | on (header default) | on (explicit) | off |
| `d3d12SDKLayers.dll` copied | yes | yes | no |

Rationale:

- **Debug is too slow for a path tracer** to be useful beyond stepping through CPU code;
  the debug layer and unoptimized host code make frames take long enough that
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

## Windows build with a sanitized environment

**Use this only after the normal `cmake --build` command above fails.** Codex's Windows host
can expose case variants of the same environment variable, such as `Path` and `PATH`, to child
processes. Windows treats those names as equivalent, but a child process can still receive both
entries. MSBuild rejects the duplicate key before compilation starts. During configuration, the
same problem can also make CMake `try_compile` fail, causing FastSIMD to misleadingly report
`unknown` / `SCALAR` and create `FastSIMD_FastNoise` with no sources.

Run the normal CMake build in a child process whose environment is copied after removing all
case variants of `Path`, `TEMP`, and `TMP`, then add those three canonical keys exactly once.
Clearing `ProcessStartInfo.Environment` first is important: otherwise it starts with the inherited
entries and assigning `Path` does not necessarily remove a separate `PATH` entry. Keep compiler
temporary files inside the build directory:

```powershell
$tempDir = Join-Path (Get-Location) 'build/msbuild-temp'
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = $cmake
$psi.WorkingDirectory = (Get-Location).Path
$psi.UseShellExecute = $false
$psi.Environment.Clear()

foreach ($entry in [System.Environment]::GetEnvironmentVariables().GetEnumerator()) {
    if ($entry.Key -inotmatch '^(path|temp|tmp)$') {
        $psi.Environment[$entry.Key] = $entry.Value
    }
}
$psi.Environment['Path'] = $env:PATH
$psi.Environment['TEMP'] = $tempDir
$psi.Environment['TMP'] = $tempDir

@(
    '--build'
    'build'
    '--config'
    'RelWithDebInfo'
    '--target'
    'Biomeinator'
) | ForEach-Object { [void]$psi.ArgumentList.Add($_) }

$buildProcess = [System.Diagnostics.Process]::Start($psi)
$buildProcess.WaitForExit()
exit $buildProcess.ExitCode
```

The child still executes the same `cmake --build` workflow, so CMake regeneration, generated
shaders, project dependencies, and MSVC's normal incremental dependency tracking all remain
active. A change to a shared header must therefore rebuild every translation unit that includes
it. If that does not happen, run once with `--clean-first` and investigate the dependency state;
the sanitized environment itself should not change which sources are considered out of date.
