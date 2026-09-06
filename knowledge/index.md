_Last edited: 2026-09-06_

# Knowledgebase Index

Each subsystem has its own folder with a local `index.md` that describes its entries in more detail.

| Folder | Contents |
|---|---|
| [rendering/](rendering/index.md) | Render passes, pipeline, camera, frame management, DLSS, NVAPI |
| [gpu/](gpu/index.md) | D3D12 resource management: buffers, descriptors, acceleration structures |
| [shaders/](shaders/index.md) | HLSL shaders, utility libraries, build-time compilation |
| [restir/](restir/index.md) | ReSTIR PT: design decisions and staged build-up towards ReSTIR PT Enhanced |
| [scene/](scene/index.md) | Scene graph, instances, materials, textures, glTF loading |
| [terrain/](terrain/index.md) | Terrain generation, chunks, biomes, structures, meshing |
| [multithreading/](multithreading/index.md) | Thread pool, parallel chunk generation pipeline |
| [settings/](settings/index.md) | Runtime settings and CLI argument parsing |
| [util/](util/index.md) | Math helpers, RNG, Halton sequence, ring buffer |
| [build/](build/index.md) | CMake configurations, third-party dependencies |
| [debugging/](debugging/index.md) | GPU fault diagnosis: Aftermath crash dumps, and instrumentation kept as applyable patches |
| [tests/](tests/index.md) | Golden image tests and perf runs: runner, golden image types, Blender reference renders, timing reports |
| [reference/](reference/index.md) | Vendored upstream docs (DirectX-Specs) for agent reference |
