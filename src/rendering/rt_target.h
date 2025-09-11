#pragma once

#include "dxr_includes.h"

struct RtTarget
{
private:
    ComPtr<ID3D12Resource> target{ nullptr };
    D3D12_RESOURCE_DESC targetResourceDesc{};
    D3D12_RESOURCE_STATES targetResourceState{ D3D12_RESOURCE_STATE_COMMON };

    struct
    {
        uint32_t idx{ ~0u };
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    } uav;

    struct
    {
        uint32_t idx{ ~0u };
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    } srv;

public:
    const bool hasUav;
    const bool hasSrv;

    RtTarget(DXGI_FORMAT format, bool hasUav, bool hasSrv);

    void setDimensions(uint32_t width, uint32_t height);

    void init();
    void reset();

    void transitionToState(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES newState);

    ID3D12Resource* getTarget() const;

    uint32_t getUavIdx() const;
    uint32_t getSrvIdx() const;
};
