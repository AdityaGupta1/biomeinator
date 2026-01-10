# Biomeinator

Real-time path traced voxel engine

https://github.com/user-attachments/assets/f11fb26e-f6b5-47eb-b903-684f72c198c3

(this is a test scene, not procedurally generated yet)

## Building

Make sure to clone with `--recurse-submodules` to gather all required dependencies.

Then, you should be able to just open the folder with Visual Studio 2022 and have it automatically recognize the CMake project.

Or, you can:

- Run `setup.bat`
  - You probably need CMake installed for this to work
- Load the Visual Studio solution at `build/Biomeinator.sln`
- Right-click "Biomeinator" in the Solution Explorer and set as default startup project
- Build and run

Once the project is running, you can open a glTF scene from `test_scenes/` with <kbd>Ctrl</kbd> + <kbd>O</kbd>. Use <kbd>WASD</kbd> to move horizontally, <kbd>Q</kbd> and <kbd>E</kbd> to move vertically, and the mouse to rotate. You can also press <kbd>Z</kbd> to toggle the cursor for accessing the settings menu, hold <kbd>C</kbd> to zoom, or press <kbd>P</kbd> to take a screenshot (which is then stored in `Documents/biomeinator/screenshots/`).

## Third-Party Licenses

This project uses various third-party libraries:

- [tinygltf](https://github.com/syoyo/tinygltf) - MIT
- [json](https://github.com/nlohmann/json) - MIT
- [stb](https://github.com/nothings/stb) - MIT
- [cxxopts](https://github.com/jarro2783/cxxopts) - MIT
- [Dear ImGui](https://github.com/ocornut/imgui) - MIT
- [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) - University of Illinois Open Source
- [DirectX-Headers](https://github.com/microsoft/DirectX-Headers) - MIT
- [Agility SDK](https://devblogs.microsoft.com/directx/directx12agility/) - Microsoft Software License
- [Minimal AgX implementation](https://iolite-engine.com/blog_posts/minimal_agx_implementation) - MIT
- [Khronos PBR Neutral tonemapper](https://github.com/KhronosGroup/ToneMapping) - Apache-2.0
- [NVAPI](https://github.com/NVIDIA/nvapi) - MIT
- [Streamline](https://github.com/NVIDIA-RTX/Streamline) - MIT

Third-party license text files are available in their respective folders in `external/`. Licenses without specific folders are in `external/_licenses`.

Block textures come from the [Good Vibes](https://github.com/Phyronnaz/VoxelAssets/tree/master/GoodVibes) texture pack by Acaitart.
