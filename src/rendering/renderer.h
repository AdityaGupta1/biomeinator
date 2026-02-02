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
#include "buffer/descriptor_heap_allocator.h"

#include <string>

class Camera;
class Scene;

namespace Renderer
{

void init();

void loadScene(const std::string& filePathStr);

void resize();

void render();

void queueScreenshot(const bool useTestOutputPath = false);

void flush();

void destroy();

inline constexpr uint32_t NUM_FRAMES_IN_FLIGHT = 3;
uint32_t getFrameIndex();

extern ComPtr<ID3D12Device5> device;

extern DescriptorHeapAllocator sharedDescHeapAlloc;

const Camera& getCamera();

const Scene& getScene();

} // namespace Renderer
