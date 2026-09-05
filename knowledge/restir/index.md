_Last edited: 2026-09-04_

# ReSTIR PT

Spatiotemporal path reuse, built towards ReSTIR PT Enhanced (Lin, Kettunen, Wyman 2026) one
verifiable step at a time. Shaders live in `src/shaders/restir/`; it is the `RESTIR_PT` sampling mode
(`--samplingMode=3`), which always uses RTSL for NEE.

| Entry | Description |
|---|---|
| [design.md](design.md) | What is and is not a resampled path, reservoir contents and their semantics, replay, RNG streams, reconnection criteria |
| [pairing_textures.md](pairing_textures.md) | Self-inverting neighbor pairing for paired spatial reuse: generation, tiling, per-frame transform, unit checks |
