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

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut vsMain(uint id : SV_VertexID)
{
    // (-1,-1), (3,-1), (-1,3) covers screen without diagonal seam
    float2 pos = (id == 0) ? float2(-1, -1) :
                 (id == 1) ? float2(3, -1) :
                              float2(-1, 3);
    VsOut vsOut;
    vsOut.pos = float4(pos, 0, 1);
    vsOut.uv = 0.5f * pos + 0.5f; // map clip-space to [0,1]
    return vsOut;
}

