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

#pragma once

#include "../rendering/common/common_structs.h"

#include "global_params.hlsli"

#define RAY_ORIGIN_OFFSET_EPSILON 0.00001f

RaytracingAccelerationStructure raytracingAcs : REGISTER_T(RT_REGISTER_RAYTRACING_ACS, RT_REGISTER_SPACE_BUFFERS);

StructuredBuffer<InstanceData> instanceDatas : REGISTER_T(RT_REGISTER_INSTANCE_DATAS, RT_REGISTER_SPACE_BUFFERS);

ByteAddressBuffer idxs : REGISTER_T(RT_REGISTER_IDXS, RT_REGISTER_SPACE_BUFFERS);

void loadVertsFromInstance(const InstanceData instanceData, const uint triIdx, out Vertex v0, out Vertex v1, out Vertex v2)
{
    uint i0, i1, i2;
    if (bool(instanceData.hasIdxs))
    {
        const uint idxsBufferByteOffset = instanceData.idxsBufferByteOffset + triIdx * 3 * 4;
        i0 = idxs.Load(idxsBufferByteOffset + 0);
        i1 = idxs.Load(idxsBufferByteOffset + 4);
        i2 = idxs.Load(idxsBufferByteOffset + 8);
    }
    else
    {
        i0 = triIdx * 3;
        i1 = i0 + 1;
        i2 = i0 + 2;
    }

    StructuredBuffer<Vertex> verts = ResourceDescriptorHeap[heapIndices.srv.vertsIdx];

    v0 = verts[instanceData.vertsBufferOffset + i0];
    v1 = verts[instanceData.vertsBufferOffset + i1];
    v2 = verts[instanceData.vertsBufferOffset + i2];
}
