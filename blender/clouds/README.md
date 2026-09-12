# Cloud reference

`clouds_ref.blend` is the user's source, with `cam_top` and `cam_bottom`.
The source file has not been modified. The basic shape is now implemented;
the user will tune the final appearance in game.

## Graph and default parameters

Geometry Position is scaled by 0.07. Blender Z becomes engine Y.

1. Normalized 2D fBM (scale 2.5, detail 2, roughness 0.5, lacunarity 2)
   supplies Color RG, added to XY with strength 1, without subtracting 0.5.
2. 2D Euclidean Smooth F1 Voronoi uses scale 1, detail 0, randomness 2/3,
   and smoothness 1. Blender halves the smoothness internally.
3. Mapped Z / 10 feeds an EASE ramp from black at 0 to white at 0.17727293,
   multiplied by 6.1 and added to the Voronoi distance.
4. Normalized 3D fBM (scale 30, detail 2, roughness 0.5, lacunarity 2)
   contributes another 0.1 times its Fac.
5. A LINEAR ramp maps white at 0.145454556 to black at 0.363636315.
   The implementation reproduces Cycles' 256 ramp intervals.

The lower boundary is deliberately hard. No bottom fade, extra weather mask,
cellular erosion, or additional taper is applied. Lighting continues to use
the engine's volumetric approximation, rather than Cycles Principled Volume.

## In-game controls

**Atmosphere → Cloud settings** contains **Cloud shape**, **Surface detail**,
and **Height and density** groups. All 14 graph controls have matching CLI
options, participate in cache/accumulation invalidation, and are included in
**Copy cloud settings**. **Reset cloud defaults** restores the latest tuned preset.
The earlier shape/erosion CLI options have been replaced by these graph controls.

Noise scale sliders snap internally to 1/16 so the periodic lattice stays
seamless. Keep the density ramp black position above its white position;
the renderer enforces a minimum separation of 0.001. Coverage 0.5 reproduces
the reference threshold; other coverage values shift that threshold.

Default layer thickness 1500 corresponds to 150 game meters per Blender unit.
The tile spans 16 mapped units, so its matching size is 34285.714 m; extinction
is 1/150 per meter. For another thickness, preserve proportions with
`cloudPeriod = cloudThickness * 160 / 7`, and preserve optical depth with
`cloudDensity = 10 / cloudThickness`.

## Implementation and checks

The 2D distortion and Smooth F1 are cached in 1024×1×1024 textures. Height
and fine noise are evaluated at march samples. The light cache uses the same
density function. The cache lattice wraps beyond the central reference region
and retains the existing rotation, wind, bounded marching, and sunset fixes.
The engine repeats the layer horizontally instead of clipping it to Blender's
150×150 bounding mesh.

The noise adaptation is Apache-2.0, from Blender 4.4.1 Cycles; see the shader
header and `external/_licenses/LICENSE_cycles.txt`.

RelWithDebInfo and all shader variants built. A standalone D3D12 compute check
compared 256 samples against the original graph rendered as numeric emission
patches in Cycles. Maximum absolute density error was 0.000024 before spatial
cache filtering. Both reference cameras and translated engine views rendered;
a further smoke capture exercised altered graph controls. Validation artifacts
and scripts are in `build/cloud-reference/`, not committed. These checks do not
establish final visual quality, live slider interaction, or performance.
