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

#pragma once

#include "dxr_includes.h"

class Fence
{
private:
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue{ 0 };
    HANDLE fenceEvent{ nullptr };

public:
    void init();

    uint64_t signal(ID3D12CommandQueue* cmdQueue);

    void waitFor(uint64_t waitFenceValue);

    void reset();
};
