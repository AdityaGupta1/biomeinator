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

VsOut vsMain(uint vertIdx : SV_VertexID)
{
    // TODO: remove branching
    float2 pos = (vertIdx == 0) ? float2(-1, 3) :
                 (vertIdx == 1) ? float2(3, -1) :
                                  float2(-1, -1);
    VsOut vsOut;
    vsOut.pos = float4(pos, 0, 1);
    vsOut.uv = pos * float2(0.5f, -0.5f) + 0.5f;
    return vsOut;
}

