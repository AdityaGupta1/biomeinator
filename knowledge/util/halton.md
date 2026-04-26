_Last edited: 2026-04-26_

# Halton Sequence

`src/util/halton.h/cpp` — low-discrepancy quasi-random 2D sequence used for TAA jitter and camera sub-pixel offsets.

## Why Not Random

Uniform random jitter clusters and leaves gaps. Halton sequences guarantee even coverage over time — each new sample fills the largest remaining gap. This means temporal accumulation converges faster (fewer frames to a noise-free image) than with white noise jitter.

## Precomputation

The sequence is generated once at init into a fixed-size vector, then cycled through via `next()` with a wrapping pointer. No per-frame computation. Bases 2 and 3 produce the X and Y dimensions respectively.

## Usage

Used by the camera system to offset the projection matrix sub-pixel each frame. The path tracer accumulates these jittered frames, and DLSS uses the jitter offsets for its temporal reconstruction.
