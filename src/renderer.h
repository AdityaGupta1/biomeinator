#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <algorithm>
#include <DirectXMath.h>
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace Renderer
{
	void init();

	LRESULT WINAPI onWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	void initDevice();
	void initSurfaces(HWND hwnd);
	void resize(HWND hwnd);
	void initCommand();
	void initMeshes();
	void initBottomLevel();
	void initScene();
	void updateTransforms();
	void initTopLevel();
	void initRootSignature();
	void initPipeline();

	void render();
};