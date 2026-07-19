# Biomeinator

Real-time path traced voxel engine

![](img/title.png)

Video demo: https://youtu.be/6ehg5h1aBRI

## Building

Make sure to clone with `--recurse-submodules` to gather all required dependencies.

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
- Press <kbd>P</kbd> to take a screenshot (which is then stored in `Documents/biomeinator/screenshots/`)
- Press <kbd>H</kbd> to toggle GUI visibility

## Third-Party Licenses

This project uses various third-party libraries:

- [tinygltf](https://github.com/syoyo/tinygltf) - MIT
- [json](https://github.com/nlohmann/json) - MIT
- [stb](https://github.com/nothings/stb) - MIT
- [cxxopts](https://github.com/jarro2783/cxxopts) - MIT
- [Dear ImGui](https://github.com/ocornut/imgui) - MIT
- [ImPlot](https://github.com/epezent/implot) - MIT
- [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) - University of Illinois Open Source
- [DirectX-Headers](https://github.com/microsoft/DirectX-Headers) - MIT
- [Agility SDK](https://devblogs.microsoft.com/directx/directx12agility/) - Microsoft Software License
- [Minimal AgX implementation](https://iolite-engine.com/blog_posts/minimal_agx_implementation) - MIT
- [Khronos PBR Neutral tonemapper](https://github.com/KhronosGroup/ToneMapping) - Apache-2.0
- [NVAPI](https://github.com/NVIDIA/nvapi) - MIT
- [Streamline](https://github.com/NVIDIA-RTX/Streamline) - MIT (DLSS binaries are under the [NVIDIA RTX SDKs License](external/streamline/bin/x64/nvngx_dlss.license.txt))
- [NRC](https://github.com/NVIDIA-RTX/NRC) - NVIDIA RTX SDKs License
- [GLM](https://github.com/g-truc/glm/tree/master) - MIT
- [FastNoiseLite](https://github.com/Auburn/FastNoiseLite) - MIT
- [FastNoise2](https://github.com/Auburn/FastNoise2) - MIT
- [GPUSorting](https://github.com/b0nes164/GPUSorting) - MIT
- [LZ4](https://github.com/lz4/lz4) - BSD 2-Clause

Third-party license text files are available in their respective folders in `external/`. Licenses without specific folders are in `external/_licenses`.

Reference documentation vendored under `reference/` (not compiled into the project):

- [DirectX-Specs](https://github.com/microsoft/DirectX-Specs) - docs under CC BY 4.0, code samples under MIT

Block textures come from:
- [Good Vibes](https://github.com/Phyronnaz/VoxelAssets/tree/master/GoodVibes) by Acaitart
- [16px Seamless MC Texture Hyperpack](https://reactorcore.itch.io/16px-seamless-mc-texture-hyperpack) by Reactorcore

This project is not sponsored by, endorsed by, or affiliated with NVIDIA Corporation.
