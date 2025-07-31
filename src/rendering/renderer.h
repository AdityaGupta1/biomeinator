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

#include "dxr_includes.h"

#include <string>

namespace Renderer
{

void init();

void loadGltf(const std::string& filePathStr);

void resize();

void render();

void flush();

void queueScreenshot();

extern ComPtr<ID3D12Device5> device;

extern ComPtr<ID3D12DescriptorHeap> sharedHeap;

} // namespace Renderer
