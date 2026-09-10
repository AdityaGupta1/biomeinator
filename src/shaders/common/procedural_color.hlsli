// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "util/color.hlsli"
#include "util/math.hlsli"

// Faces flagged TRIANGLE_FLAG_PROCEDURAL_COLOR multiply their emission by a world-space ramp;
// diffuse and transmission keep their texture color: the hue sweeps green -> magenta -> green along the (1, 1, 1) diagonal and drifts
// with time. Evaluating it from the shading point rather than baking it per triangle is what keeps
// it smooth within a single block and lets it animate, and it makes the ramp independent of the
// geometry, so a crystal model gets it on the same terms as a cube.
//
// It must be evaluated at the same world position by every path that shades a surface, or NEE and
// BSDF hits disagree about an emitter's color and MIS combines two different colors.

static const float3 proceduralColorAxis = float3(0.57735f, 0.57735f, 0.57735f); // normalize(1, 1, 1)
static const float proceduralColorBlocksPerCycle = 16.f;
static const float proceduralColorCyclesPerSecond = 0.015f;
static const float proceduralColorHueStart = 120.f / 360.f;   // green
// Past magenta rather than at it: the sweep turns around in rose, so it passes through magenta
// twice and that end of the ramp is a band rather than an instant
static const float proceduralColorHueEnd = 320.f / 360.f;
static const float proceduralColorSaturation = 0.55f;
// Every hue on the ramp is scaled to this luminance. At a fixed value the sweep's own luminance
// swings about eightfold (green is bright, the blue midpoint is very dark), which reads as the
// clusters pulsing in brightness as the ramp drifts rather than changing color. Equalizing pushes
// the dark hues' channels well above 1, which is why this is only for emission.
static const float proceduralColorLuminance = 1.8f;

float3 getProceduralColor(const uint triangleFlags, const float3 pos_WS)
{
    if (!bool(triangleFlags & TRIANGLE_FLAG_PROCEDURAL_COLOR))
    {
        return float3(1.f, 1.f, 1.f);
    }

    const float3 blocks_WS = pos_WS + float3(cameraParams.globalInstanceOffset);
    const float phase = dot(blocks_WS, proceduralColorAxis) / proceduralColorBlocksPerCycle +
                        renderParams.animTime * proceduralColorCyclesPerSecond;
    // Triangle wave, so the hue sweeps at a constant rate and turns around at the two endpoints
    // rather than dwelling on them as a cosine would
    const float rampT = abs(frac(phase) * 2.f - 1.f);
    const float hue = lerp(proceduralColorHueStart, proceduralColorHueEnd, rampT);

    const float3 rampColor = srgbToLinear(hsvToRgb(float3(hue, proceduralColorSaturation, 1.f)));
    // Equalizing pushes the dark hues' channels above 1, which is meaningless for an emitter but
    // would be energy-gaining as a transmission tint; renormalize before putting this flag on a
    // block that isn't a pure emitter
    return rampColor * (proceduralColorLuminance / luminance(rampColor));
}
