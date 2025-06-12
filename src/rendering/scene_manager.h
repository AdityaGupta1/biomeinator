#pragma once

#include "dxr_includes.h"

#include "common_structs.h"
#include "buffer/acs_helper.h"

class ToFreeList;

struct Instance
{
private:
    const uint32_t id{ -1u };
    AcsHelper::GeometryWrapper geoWrapper;

public:
    std::vector<Vertex> host_verts;
    std::vector<uint32_t> host_idxs;
};

namespace SceneManager
{

constexpr uint32_t MAX_INSTANCES = 3; // will be more than NUM_INSTANCES after adding chunks
constexpr uint32_t NUM_INSTANCES = 3;

void init(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList);

ID3D12Resource* getDevInstanceDescs();
ID3D12Resource* getDevInstanceDatas();

ID3D12Resource* getDevTlas();

ID3D12Resource* getDevVertBuffer();
ID3D12Resource* getDevIdxBuffer();

} // namespace SceneManager
