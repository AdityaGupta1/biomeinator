#pragma once

float luminance(const float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}
