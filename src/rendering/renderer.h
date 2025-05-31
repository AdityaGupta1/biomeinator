#pragma once

#include "dxr_includes.h"

#include <algorithm>

namespace Renderer
{
	void init();

	void render();

	void flush();

	extern ComPtr<ID3D12Device5> device;
	extern ComPtr<ID3D12CommandAllocator> cmdAlloc;
	extern ComPtr<ID3D12GraphicsCommandList4> cmdList;
	extern ComPtr<ID3D12CommandQueue> cmdQueue;
};
