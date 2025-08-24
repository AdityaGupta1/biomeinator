#pragma once

float3 applyKhronosPbrNeutral(float3 color) {
    constexpr float startCompression = 0.8f - 0.04f;
    constexpr float desaturation = 0.15f;

    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
    {
        return color;
    }

    constexpr float d = 1.f - startCompression;
    const float newPeak = 1.f - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    const float g = 1.f - 1.f / (desaturation * (peak - newPeak) + 1.f);
    return lerp(color, float3(newPeak), g);
}
