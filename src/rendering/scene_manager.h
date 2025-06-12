#pragma once

#include "dxr_includes.h"
#include "common_structs.h"

class ToFreeList;

namespace SceneManager
{

constexpr uint32_t MAX_INSTANCES = 3; // will be more than NUM_INSTANCES after adding chunks
constexpr uint32_t NUM_INSTANCES = 3;

void init(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

void update(ID3D12GraphicsCommandList4* cmdList);

ID3D12Resource* getDevInstanceDescs();
ID3D12Resource* getDevInstanceDatas();

ID3D12Resource* getDevTlas();

ID3D12Resource* getDevVertBuffer();
ID3D12Resource* getDevIdxBuffer();

} // namespace SceneManager
