// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

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

extern DescriptorHeapAllocator sharedDescHeapAlloc;

ID3D12Device5* getDevice();
ID3D12CommandQueue* getGraphicsQueue();

const Camera& getCamera();

const Scene& getScene();

} // namespace Renderer
