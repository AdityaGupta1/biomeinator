_Last edited: 2026-04-25_

# Settings Manager

`src/settings_manager.h/cpp` — a global, stringly-typed key-value store for all runtime settings. Parsed once from CLI args at startup via `parseArgs()`, then readable and writable from anywhere at any time (including mid-frame from the GUI).

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
toggleBool("nrcEnabled") // convenience flip
```

## How Settings Reach the GPU

Settings are not passed to shaders directly. Each frame the renderer reads the relevant settings and populates the param structs defined in `common_params.h` (`RenderParams`, `SceneParams`, `DebugParams`, etc.), which are uploaded as a constant buffer. See [shaders → common_structs.md](../shaders/common_structs.md) for the full param struct layout.

## Notable Settings

All settings and their defaults are defined in `parseArgs()` and are self-describing. A few non-obvious ones:

- **`voxelMode`** (default `false`): The main switch between the two rendering modes. `true` = procedural voxel terrain; `false` = load a glTF scene specified by `--scene`. Voxel terrain is the primary purpose of the project.
- **`debugBool0–3` / `debugFloat0–3`**: Passed to shaders every frame. Useful for tweaking shader behaviour on the fly without recompiling — wire them up temporarily to any shader constant while iterating.
- **`testOutput`**: If set to a `.png` path, the engine renders one frame, saves a screenshot, and exits. Used by automated tests.
- **`lockCamera`**: Disables player input; useful for test screenshots to get a reproducible viewpoint.

## GUI Helpers

`src/settings_gui_helpers.h` provides thin ImGui wrappers in the `SettingsGuiHelpers` namespace that read/write settings and handle clamping:

- `Checkbox`, `InputInt`, `SliderInt`, `InputUint`, `SliderUint`, `ComboUint`, `SliderFloat`, `ComboString`
- All return `bool` indicating whether the value changed this frame.
- `ScopedItemWidth` is a RAII helper for `ImGui::PushItemWidth` / `PopItemWidth`.
