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
#include "path_tracing_common.hlsli"
#include "restir.hlsli"
#include "util/rng.hlsli"

StructuredBuffer<RisSample> risSamplesIn : REGISTER_T(TEMPORAL_REUSE_REGISTER_RIS_SAMPLES_IN, TEMPORAL_REUSE_REGISTER_SPACE);
StructuredBuffer<RisSample> risSamplesPrev : REGISTER_T(TEMPORAL_REUSE_REGISTER_RIS_SAMPLES_PREV, TEMPORAL_REUSE_REGISTER_SPACE);

RWStructuredBuffer<RisSample> risSamplesOut : REGISTER_U(TEMPORAL_REUSE_REGISTER_RIS_SAMPLES_OUT, TEMPORAL_REUSE_REGISTER_SPACE);

[shader("compute")]
[numthreads(TEMPORAL_REUSE_WORKGROUP_SIZE_X, TEMPORAL_REUSE_WORKGROUP_SIZE_Y, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixelIdx = dispatchThreadId.xy;

    if (any(pixelIdx >= renderParams.renderSize))
    {
        return;
    }

    const uint linearPixelIdx = pixelIdx.y * renderParams.renderSize.x + pixelIdx.x;

    const RisSample thisRisSample = risSamplesIn[linearPixelIdx];
    if (thisRisSample.lightIdx == LIGHT_IDX_INVALID)
    {
        risSamplesOut[linearPixelIdx] = thisRisSample;
        return;
    }

    RandomSampler rng = initRandomSampler(constantParams.rngSeed ^ 44721359, linearPixelIdx, renderParams.frameNumber);

    // TODO: temporal reuse

    RisSample risSampleOut;
    risSampleOut = risSamplesIn[linearPixelIdx]; // TODO: fill out risSampleOut
    //risSampleOut.pad0 = risSampleOut.pad1 = risSampleOut.pad2 = 0;
    risSamplesOut[linearPixelIdx] = risSampleOut;
}
