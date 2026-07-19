_Last edited: 2026-07-18_

# DirectX-Specs

Full checkout of [microsoft/DirectX-Specs](https://github.com/microsoft/DirectX-Specs)
at `reference/DirectX-Specs/` (shallow submodule, `shallow = true` in
`.gitmodules`). These are the authoritative Microsoft engineering specs for
D3D12 and HLSL features — the source of truth behind MSDN, usually more
detailed and more current than the public docs.

## How to use it

Prefer grepping this local checkout over web searches or WebFetch on the
GitHub site: every spec is a single markdown file, so a repo-wide grep finds
the relevant one instantly and lets you read the whole thing in context.
Almost all specs live in `reference/DirectX-Specs/d3d/`; `d3d/archive/`
holds superseded drafts and `d3d/images/` the figures.

## Why vendored, and where

Placed under top-level `reference/`, not `external/`, because it is
documentation, not compiled code — `external/` is reserved for build
dependencies. Vendored as a submodule (rather than a knowledgebase entry that
just links out) so agents can grep across all specs at once and read them
offline against a pinned revision. Shallow because history has no value for
reference docs.

## Specs most relevant to this renderer

`Raytracing.md` and `Raytracing2.md` (DXR — inline RT, shader tables, AS
build), `WorkGraphs.md`, `D3D12EnhancedBarriers.md`, `MeshShader.md`,
`SamplerFeedback.md`, and the `HLSL_ShaderModel6_*` / `HLSL_SM_6_*` series
(shader model feature additions). This is a DX12 path-traced voxel renderer,
so the raytracing, enhanced-barrier, and recent shader-model specs are the
ones that come up.
