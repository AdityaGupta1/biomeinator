#pragma once

#include "dxr_includes.h"

#include <algorithm>

namespace Renderer
{

void init();

void resize();

void render();

void flush();

extern ComPtr<ID3D12Device5> device;

}; // namespace Renderer
