_Last edited: 2026-08-22_

# Shader Compilation Pipeline

See [build → shader_compilation.md](../build/shader_compilation.md) for the full writeup.
This entry exists as a cross-reference since compilation is relevant to both the shader and
build knowledgebases.

## Payload access qualifiers

The ray payload (`common/payload.hlsli`) carries DXR payload access qualifiers so drivers can
shorten payload field lifetimes across shader stages (default-on for `lib_6_7+`).

- dxc 1.9 ICEs ("llvm::cast<X>() argument of incompatible type") on a method call made
  directly through a payload field (e.g. `payload.rng.nextFloat()`). Passing the field as an
  `inout` argument is fine, hence the free-function `nextFloat(payload.rng)` wrapper in
  `util/rng.hlsli`.
- One payload type serves the gbuffer, path tracing, and light-occlusion pipelines (the hit
  group shaders are shared, and payload types must match), so the qualifiers are the union of
  all pipelines' accesses. dxc's `-Wpayload-access-perf` therefore warns at the gbuffer
  TraceRay about fields only the other pipelines read back; its `-Wpayload-access-trace`
  warnings there are spurious (the analysis does not see member-wise writes, e.g. gbuffer's
  `rayCone` init).
- `hitInfo` is deliberately not `write(caller)`: after a miss it reads back undefined, which
  matches the pre-PAQ behavior (it was never initialized before TraceRay), and all
  caller-side reads are behind `PAYLOAD_FLAG_DID_HIT`.
