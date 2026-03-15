/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2026 Aditya Gupta

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

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"

#include "global_params.hlsli"

RWStructuredBuffer<uint2> rcHashEntries : REGISTER_U(RC, HASH_ENTRIES);
RWStructuredBuffer<uint4> rcAccumulation : REGISTER_U(RC, ACCUMULATION);
RWStructuredBuffer<float4> rcResolved : REGISTER_U(RC, RESOLVED);

[shader("compute")]
[numthreads(RC_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint slot = dispatchThreadId.x;

    if (slot >= RC_TABLE_SIZE)
    {
        return;
    }

    const uint2 key = rcHashEntries[slot];

    if (any(key != 0))
    {
        if (rcResolved[slot].w < RC_STALE_WEIGHT_THRESHOLD)
        {
            rcHashEntries[slot] = uint2(0, 0);
            rcResolved[slot] = float4(0, 0, 0, 0);
        }
    }

    rcAccumulation[slot] = uint4(0, 0, 0, 0);
}
