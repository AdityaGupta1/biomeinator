# Biomeinator

Real-time path traced voxel engine

![](img/title.png)

Video demo: https://youtu.be/6ehg5h1aBRI (somewhat outdated)

## Building

Make sure to clone with `--recurse-submodules` to gather all required dependencies.

For an existing checkout, run `git submodule update --init --recursive` after pulling dependency changes.
SHaRC is enabled by default on supported GPUs for interactive rendering. Use `--sharc=false` for the reference path tracer. Headless test/performance runs default to the reference path; pass `--sharc=true` to test the cache.

Then, you should be able to just open the folder with Visual Studio 2022 and have it automatically recognize the CMake project.

Or, you can:

- Install CMake if you don't have it already
- Run `setup.bat`
- Load `build/Biomeinator.sln` with Visual Studio 2022
- Right-click "Biomeinator" in the Solution Explorer and set as default startup project
- Build and run

For voxel terrain, run with the command line argument `--voxelMode`. Otherwise, you can open glTF scenes with <kbd>Ctrl</kbd> + <kbd>O</kbd>. Some example scenes are available in `test_scenes/`.

Use <kbd>WASD</kbd> to move horizontally, <kbd>Q</kbd> and <kbd>E</kbd> to move vertically, and the mouse to rotate. Additional controls include:
- Hold <kbd>C</kbd> to zoom
- Press <kbd>Z</kbd> to toggle the cursor for accessing the settings menu
- Press <kbd>F2</kbd> to take a screenshot (which is then stored in `Documents/biomeinator/screenshots/`)
- Press <kbd>H</kbd> to toggle GUI visibility
- Press <kbd>P</kbd> to pause and unpause world animation (day/night cycle, water waves)
- Hold <kbd>[</kbd> or <kbd>]</kbd> to run world animation backwards or forwards at 50x speed

## SHaRC validation

`Biomeinator.exe --sharcSelfTest --width=64 --height=64 --sharcCapacityLog2=16` runs a deterministic GPU cache test.
Use `tests/run_perf.py` with `--sharc=false` and `--sharc=true` to compare total GPU cost, including update and resolve.

The settings panel includes a cache reset button and hit/bounce/grid/cached-radiance views (`--sharcDebug=1`, `2`, `3`, or `4`).
`--sharcDiagnostics=true` enables GPU counters and periodic log output. Keep it off for timing comparisons.
See [SHaRC integration notes](knowledge/rendering/sharc.md) for cache behavior and limitations.

## Third-Party Licenses

This project uses various third-party libraries:

- [tinygltf](https://github.com/syoyo/tinygltf) - MIT
- [json](https://github.com/nlohmann/json) - MIT
- [stb](https://github.com/nothings/stb) - MIT
- [cxxopts](https://github.com/jarro2783/cxxopts) - MIT
- [cpp-httplib](https://github.com/yhirose/cpp-httplib) - MIT
- [Dear ImGui](https://github.com/ocornut/imgui) - MIT
- [ImPlot](https://github.com/epezent/implot) - MIT
- [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) - University of Illinois Open Source
- [DirectX-Headers](https://github.com/microsoft/DirectX-Headers) - MIT
- [Agility SDK](https://devblogs.microsoft.com/directx/directx12agility/) - Microsoft Software License
- [Minimal AgX implementation](https://iolite-engine.com/blog_posts/minimal_agx_implementation) - MIT
- [Khronos PBR Neutral tonemapper](https://github.com/KhronosGroup/ToneMapping) - Apache-2.0
- [Blender Cycles](https://projects.blender.org/blender/blender) - Apache-2.0 (GGX energy compensation tables and the specular shading normal correction, ported into `src/shaders/util/`)
- [NVAPI](https://github.com/NVIDIA/nvapi) - MIT
- [SHaRC](https://github.com/NVIDIA-RTX/SHARC) - [NVIDIA RTX SDKs License](external/SHARC/License.md)
- [Streamline](https://github.com/NVIDIA-RTX/Streamline) - MIT (DLSS binaries are under the [NVIDIA RTX SDKs License](external/streamline/bin/x64/nvngx_dlss.license.txt))
- [GLM](https://github.com/g-truc/glm/tree/master) - MIT
- [FastNoiseLite](https://github.com/Auburn/FastNoiseLite) - MIT
- [FastNoise2](https://github.com/Auburn/FastNoise2) - MIT
- [GPUSorting](https://github.com/b0nes164/GPUSorting) - MIT
- [LZ4](https://github.com/lz4/lz4) - BSD 2-Clause

Third-party license text files are available in their respective folders in `external/`. Licenses without specific folders are in `external/_licenses`.

Reference documentation vendored under `reference/` (not compiled into the project):

- [DirectX-Specs](https://github.com/microsoft/DirectX-Specs) - docs under CC BY 4.0, code samples under MIT

Block textures come from:
- [Good Vibes](https://github.com/Phyronnaz/VoxelAssets/tree/master/GoodVibes) by Acaitart - CC-BY
- [16px Seamless MC Texture Hyperpack](https://reactorcore.itch.io/16px-seamless-mc-texture-hyperpack) by Reactorcore - CC0
- [Paler Gardens](https://modrinth.com/resourcepack/paler-gardens2) by autumnleavesfalling_2008 - MIT
- [Yuushya](https://modrinth.com/resourcepack/yuushya-16x) by Coco, Xiao2 & LD_Anvil - [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/) (textures under `assets/blocks/textures/yuushya/`; these textures and any adaptations of them are **not** covered by this project's MIT license and are for noncommercial use only)

Test scene textures in `tests/gltf/_textures/` are my own work and are **not** covered by this project's MIT license; see [tests/gltf/_textures/LICENSE.txt](tests/gltf/_textures/LICENSE.txt).

This project is not sponsored by, endorsed by, or affiliated with NVIDIA Corporation.
