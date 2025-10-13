# Biomeinator

This project aims to implement a real-time path traced voxel engine using DXR. It will also perform terrain generation using a hybrid approach between GPU and CPU, where the GPU processes expensive noise functions in parallel to place blocks while the CPU adds smaller details and builds the vertex/index buffers.

## Features

### Path Splitting

- Path splitting uses `pathSplitIdx` to deterministically evaluate different lobes: index 0 for diffuse lobes, index 1 for specular lobes.
- When a material doesn't need splitting (only has one lobe type), `pathSplitIdx == 1` should return early to avoid wasted work.
- Material splitting only occurs on the first bounce (`pathDepth == 0`) and only when path splitting is enabled.

## Coding Conventions

- Always use `const` for local variables that will not change.
- Variables called `pad#` are just for padding to a (usually 16-byte) boundary and do not need to be initialized. The one exception is if the compiler requires them to be initialized (e.g. for UAV writes).

## Formatting

- Do not run clang-format as it will break some of the existing formatting, but do look at the .clang-format file in the repository's root directory to understand how things should be formatted.
