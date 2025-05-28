#pragma once

#include "dxr_includes.h"
#include "common_structs.h"

#include <vector>

class Drawable
{
private:
	const std::vector<Vertex> verts;
	const std::vector<uint32_t> idx;

	ComPtr<ID3D12Resource> vertBuffer;
	ComPtr<ID3D12Resource> idxBuffer;

	ComPtr<ID3D12Resource> blas;

public:
	Drawable(std::vector<Vertex> verts);
	Drawable(std::vector<Vertex> verts, std::vector<uint32_t> idx);

	bool hasIdx() const;

	void initBuffers();

	ID3D12Resource* getVertBuffer();
	ID3D12Resource* getIdxBuffer();

	void initBlas();

	ID3D12Resource* getBlas();
};
