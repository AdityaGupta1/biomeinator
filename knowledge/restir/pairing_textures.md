_Last edited: 2026-09-04_

# Pairing Textures

`src/rendering/restir/pairing_texture.cpp` builds the reuse textures for paired spatial reuse
(ReSTIR PT Enhanced, Section 3). Each texel stores the wrapped delta to exactly one partner, and the
partner stores the negated delta, so the pairing is an involution: when pixel A shifts its path to
B, B's shift to A is the same evaluation and the pair costs one shift instead of two.

## Generation

Pure CPU, once at startup, no D3D dependency (so the unit tests can compile it). Link indices start
as consecutive pairs of horizontally adjacent texels, then `pairingShuffleCount(sigma)` tiled 2x2
random permutations are applied with the block grid offset diagonally every other pass. Each
index performs a random walk, so the two texels sharing an index end up a Gaussian delta apart with
standard deviation sigma. The shuffle count is the paper's fitted Eq. 3, which is essentially
sigma^2 / 2; the fit terms only matter below sigma ~2.

Deltas are wrapped to the short way round so the texture tiles, which is why the size must be even
and at most 254 (deltas fit in int8). The three production textures are 254, 230 and 210 at sigma
16, sizes chosen so no multiple of one width lands near a multiple of another within a 4K frame,
and sigma 16 matches the mean neighbor distance of the original 30-pixel uniform disk.

## Per-frame use

The textures are never regenerated. Because the pairing is self-inverting, the same texture every
frame would pair the same pixels every frame, so the consumer applies a random flip, mirror,
transpose and offset per texture per frame (the paper found this sufficient). The transform must be
applied to the delta as well as to the lookup coordinate.

## Validation

`BiomeinatorTests --unit` checks, for the three production textures: every texel's partner points
back and is never itself; deltas are wrapped; per-axis mean, standard deviation, axis correlation,
mean radial distance and the within-1-and-2-sigma fractions all match an isotropic Gaussian. The
fraction checks use the continuity-corrected expectation `erf((k*sigma + 0.5) / (sigma*sqrt(2)))`
because deltas are integers, which matters at small sigma.
