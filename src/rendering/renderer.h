#pragma once

#include "dxr_includes.h"

#include <algorithm>

namespace Renderer
{
	void init();

	LRESULT WINAPI onWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	void flush();

	void render();

	extern ComPtr<ID3D12Device5> device;
	extern ComPtr<ID3D12CommandAllocator> cmdAlloc;
	extern ComPtr<ID3D12GraphicsCommandList4> cmdList;
	extern ComPtr<ID3D12CommandQueue> cmdQueue;
};
