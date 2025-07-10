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

void saveScreenshot();

extern ComPtr<ID3D12DescriptorHeap> sharedHeap;

extern ComPtr<ID3D12Device5> device;

} // namespace Renderer
