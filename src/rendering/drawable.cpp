#include "drawable.h"

#include "dxr_common.h"
#include "renderer.h"

using namespace Renderer;

Drawable::Drawable(std::vector<Vertex> verts)
	: verts(std::move(verts)), idx()
{}

Drawable::Drawable(std::vector<Vertex> verts, std::vector<uint32_t> idx)
	: verts(std::move(verts)), idx(std::move(idx))
{}

bool Drawable::hasIdx() const
{
    return this->idx.size() > 0;
}

template<class T>
ComPtr<ID3D12Resource> initAndCopyToBuffer(const std::vector<T>& host_vector)
{
    const size_t hostDataByteSize = sizeof(T) * host_vector.size();

    auto desc = BASIC_BUFFER_DESC;
    desc.Width = hostDataByteSize;

    ComPtr<ID3D12Resource> res;
    getDevice()->CreateCommittedResource(&UPLOAD_HEAP, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&res));

    void* ptr;
    res->Map(0, nullptr, &ptr);
    memcpy(ptr, host_vector.data(), hostDataByteSize);
    res->Unmap(0, nullptr);

    return res;
}

void Drawable::initBuffers()
{
    this->vertBuffer = initAndCopyToBuffer(this->verts);
    if (this->hasIdx())
    {
        this->idxBuffer = initAndCopyToBuffer(this->idx);
    }
}

ID3D12Resource* Drawable::getVertBuffer()
{
    return this->vertBuffer.Get();
}

ID3D12Resource* Drawable::getIdxBuffer()
{
    return this->idxBuffer.Get();
}

void Drawable::initBlas()
{
    D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {
        .Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
        .Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE,

        .Triangles = {
            .Transform3x4 = 0,
            .IndexFormat = this->hasIdx() ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN,
            .VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT,
            .IndexCount = (uint32_t)this->idx.size(),
            .VertexCount = (uint32_t)this->verts.size(),
            .IndexBuffer = this->hasIdx() ? this->idxBuffer->GetGPUVirtualAddress() : 0,
            .VertexBuffer = {
                .StartAddress = this->vertBuffer->GetGPUVirtualAddress(),
                .StrideInBytes = sizeof(Vertex)
            }
        }
    };

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {
        .Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
        .Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE,
        .NumDescs = 1,
        .DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY,
        .pGeometryDescs = &geometryDesc
    };

    this->blas = makeAccelerationStructure(inputs);
}

ID3D12Resource* Drawable::getBlas()
{
    return this->blas.Get();
}
