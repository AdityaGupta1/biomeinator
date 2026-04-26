_Last edited: 2026-04-26_

# Param Blocks

`ParamBlockManager` packs all per-frame shader parameters into a single persistently-mapped
upload buffer. One instance per [frame context](frame_contexts.md).

## Single Buffer Layout

All param structs are laid out contiguously in one buffer: `HeapIndices`, `ConstantParams`,
`CameraParams`, `SceneParams`, `RenderParams`, `DebugParams`. `NrcConstants` is placed at a
separate `D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT`-aligned offset because it's bound
as a separate root CBV (NRC SDK expects its own buffer address).

## 16-Byte Alignment Invariant

Every param struct must be a multiple of 16 bytes (`static_assert`s enforce this). Violating
this causes silent GPU read corruption since D3D12 constant buffers require 16-byte-aligned
members.

## No Copy Step

The buffer lives on the upload heap and is persistently mapped. CPU writes directly to the
mapped pointer; GPU reads the same buffer via its GPU virtual address. This avoids a
per-frame upload→default copy but means the buffer must be duplicated per frame context to
avoid races.
