_Last edited: 2026-10-06_

# Settings Manager

`src/settings_manager.h/cpp` — a global, stringly-typed key-value store for all runtime settings. Parsed once from CLI args at startup via `parseArgs()`, then readable and writable from anywhere at any time (including mid-frame from the GUI).

`tryParseArgs()` contains the non-terminating parse/validation path. It builds a candidate map and
only replaces the live settings after every option and cross-option rule succeeds, which makes a
failed parse atomic and directly unit-testable. The application-facing `parseArgs()` is a thin
wrapper that preserves the command-line contract by printing help/errors and exiting when the
result is not successful.

## Storage

Settings are stored in a `static std::unordered_map<std::string, settingValue>` where `settingValue = std::variant<bool, int, uint32_t, float, std::string>`. There are no enums or structs — callers access settings by string key and must know the correct type. `worldSeed` is additionally cached as a plain `uint32_t` for fast access.

## API

```cpp
// Parsing
SettingsManager::parseArgs(argc, argv);

// Reading
getAsBool("showGui")
getAsInt("renderDistance")
getAsUint("samplingMode")
getAsFloat("movementSpeed")
getAsString("debugView")
getWorldSeed()          // fast path, no map lookup

// Writing (from GUI or runtime code)
setAsBool / setAsInt / setAsUint / setAsFloat / setAsString
toggleBool("showGui") // convenience flip
```

## How Settings Reach the GPU

Settings are not passed to shaders directly. Each frame the renderer reads the relevant settings and populates the param structs defined in `common_params.h` (`RenderParams`, `SceneParams`, `DebugParams`, etc.), which are uploaded as a constant buffer. See [shaders → common_structs.md](../shaders/common_structs.md) for the full param struct layout.

## Notable Settings

All settings and their defaults are defined in `parseArgs()` and are self-describing. A few non-obvious ones:

- **`voxelMode`** (default `false`): The main switch between the two rendering modes. `true` = procedural voxel terrain; `false` = load a glTF scene specified by `--scene`. Voxel terrain is the primary purpose of the project.
- **`renderDistance`** must be positive. CLI parsing rejects zero and negative values before window or GPU initialization, matching the world import requirement so exports cannot acquire an invalid distance through the CLI.
- **`antialiasingMode`**: Defaults to `DLSS` in voxel mode, `NONE` otherwise. Anything wanting a
  deterministic mode (e.g. the test runner) must pass `--antialiasingMode` explicitly.
- **`debugBool0–3` / `debugFloat0–3`**: Passed to shaders every frame. Useful for tweaking shader behaviour on the fly without recompiling — wire them up temporarily to any shader constant while iterating.
- **`renderToFile`**: If set to a `.png` path, the engine accumulates to `maxAccumulatedFrames` in a never-shown window, saves a screenshot, and exits. Used by the rendering tests and by agents that need an image of a scene or world without a window popping up.
- **Generated-world camera arguments** (`cameraX/Y/Z`, `cameraYaw/Pitch`) make procedural
  terrain screenshots reproducible without exporting a world. Angles use degrees, yaw zero
  points along +Z, and positive pitch looks up. They only initialize voxel mode; an imported
  world's saved camera still takes precedence, and glTF cameras are unaffected. `fovY` sets the
  field of view, for reproducing a capture taken with zoom held.
- **`perfOutput`**: If set to a `.json` path, the engine warms up, measures `perfFrames` frames, writes GPU timing statistics, and exits. Mutually exclusive with `renderToFile`. See [tests → perf_runs.md](../tests/perf_runs.md).
- **`isAutomatedRun()`** is true for either of the above and is what code should test for "automated run" behaviour (await voxel import); `isRenderToFileMode()` and `isPerfMode()` are for the behaviour specific to each, such as the render-to-file window staying hidden while a perf run's comes to the foreground. An automated run also defaults `lockCamera`, `showGui`, `animTimePaused` and `useVsync` to a fixed, unanimated, unthrottled viewpoint, `sharc` off (see [rendering → sharc.md](../rendering/sharc.md)) and `lodDistance` to 0 (no terrain LODs), but only when they were not passed explicitly. This is the single place those defaults live; the rendering test runner and `run_perf.py` pass only their output path.
- **`forEachSetting`** exists so a perf report can embed every setting it ran with; there is no other reason to enumerate the map.
- **`lockCamera`**: Disables player input; useful for test screenshots to get a reproducible viewpoint.
- **`animTimePaused`** (default `false`): Freezes only the animation time driving world animation
  (water displacement and shading, sun position); player movement, camera, and everything else
  timed stay unaffected. Runtime toggling goes through the setting rather than renderer-side
  state, so there is no pause flag to keep in sync. Automated runs default it to `true` so
  goldens are deterministic. Scrub keys still move
  time while paused — see [rendering → render_loop.md](../rendering/render_loop.md).

## GUI Helpers

`src/settings_gui_helpers.h` provides thin ImGui wrappers in the `SettingsGuiHelpers` namespace that read/write settings and handle clamping:

- `Checkbox`, `InputInt`, `SliderInt`, `InputUint`, `SliderUint`, `ComboUint`, `SliderFloat`, `ComboString`
- All return `bool` indicating whether the value changed this frame.
- `ScopedItemWidth` is a RAII helper for `ImGui::PushItemWidth` / `PopItemWidth`.

The Settings window (`renderer_gui.cpp`) is a tab bar with one `draw*Tab()` function per tab. Only the selected tab's widgets are submitted, which is safe for the change flags because a widget can only report a change on a frame it is drawn and edited. Labels inside a tab are short (e.g. "Anisotropy" under both Clouds and Fog), so sections that repeat labels wrap themselves in `ScopedId`; without it ImGui would give both sliders the same ID and they would fight over input. The window has a fixed width and auto-fits its height up to the space above the Performance window, so switching tabs does not make it jump sideways and a long tab scrolls instead of running under the frame time plot.
