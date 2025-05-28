#pragma once

#include "dxr_includes.h"

#include <algorithm>

namespace Renderer
{
	void init();

	LRESULT WINAPI onWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	void initDevice();
	void initSurfaces(HWND hwnd);
	void resize(HWND hwnd);
	void initCommand();
	void initMeshes();
	ComPtr<ID3D12Resource> makeAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, UINT64* updateScratchSize = nullptr);
	void initBottomLevel();
	void initScene();
	void updateTransforms();
	void initTopLevel();
	void initRootSignature();
	void initPipeline();

	void render();

	ID3D12Device5* getDevice();
};
