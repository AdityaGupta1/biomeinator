/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "../rendering/common/common_enums.h"
#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "global_params.hlsli"

StructuredBuffer<float4> pathTracingRawBufferIn : REGISTER_T(COLLECT_REGISTER_PATH_TRACING_RAW_BUFFER_IN, COLLECT_REGISTER_SPACE);

[shader("compute")]
[numthreads(COLLECT_WORKGROUP_SIZE_X, COLLECT_WORKGROUP_SIZE_Y, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixelIdx = dispatchThreadId.xy;

    if (pixelIdx.x >= renderParams.renderSize.x || pixelIdx.y >= renderParams.renderSize.y)
    {
        return;
    }

    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;

    float4 color;
    if (renderParams.enablePathSplitting)
    {
        const float4 color0 = pathTracingRawBufferIn[linearPixelIdx * 2];
        const float4 color1 = pathTracingRawBufferIn[linearPixelIdx * 2 + 1];
        color = color0 + color1;
    }
    else
    {
        color = pathTracingRawBufferIn[linearPixelIdx];
    }

    if ((AntialiasingMode) renderParams.antialiasingMode == AntialiasingMode::ACCUMULATE)
    {
        color.rgb /= (renderParams.accumulatedFrameNumber + 1.f);
    }

    RWTexture2D<float4> pathTracingTarget = ResourceDescriptorHeap[heapIndices.uav.pathTracingTargetIdx];
    pathTracingTarget[pixelIdx] = color;
}

