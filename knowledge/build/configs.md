_Last edited: 2026-09-08_

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

## Windows fallback for duplicate environment paths

**Use this only after the normal `cmake --build` command above fails.** Codex's Windows host
can expose case variants of the same environment variable (for example `Path` and `PATH`, or
`TEMP` and `TMP`) to child processes. MSBuild and CMake `try_compile` then fail with a duplicate
environment-path error, or FastSIMD misleadingly reports `unknown` / `SCALAR` and creates
`FastSIMD_FastNoise` with no sources.

For an existing `build/` Visual Studio tree, launch MSBuild with those case-insensitive keys
removed and re-added exactly once. Keep compiler temporary files inside the build directory:

```powershell
$tempDir = Join-Path (Get-Location) 'build/msbuild-temp'
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
$psi.WorkingDirectory = (Get-Location).Path
$psi.UseShellExecute = $false

foreach ($entry in [System.Environment]::GetEnvironmentVariables().GetEnumerator()) {
    if ($entry.Key -inotmatch '^(path|temp|tmp)$') {
        $psi.Environment[$entry.Key] = $entry.Value
    }
}
$psi.Environment['Path'] = $env:PATH
$psi.Environment['TEMP'] = $tempDir
$psi.Environment['TMP'] = $tempDir

@(
    'build/Biomeinator.vcxproj'
    '/m:2'
    '/p:Configuration=RelWithDebInfo'
    '/p:Platform=x64'
    '/p:BuildProjectReferences=false'
    '/v:minimal'
) | ForEach-Object { [void]$psi.ArgumentList.Add($_) }

$buildProcess = [System.Diagnostics.Process]::Start($psi)
$buildProcess.WaitForExit()
exit $buildProcess.ExitCode
```

This is a recovery path, not the default build command. `BuildProjectReferences=false` relies on
the dependencies already present in the configured build tree; return to the normal CMake command
when dependencies or generated project structure need a full rebuild.
