#pragma once

#include <DirectXMath.h>

struct PlayerInput
{
    DirectX::XMFLOAT3 linearInput{ 0, 0, 0 };
    float linearSpeedMultiplier{ 1.f };
    DirectX::XMFLOAT2 mouseMovement{ 0, 0 };
    bool isZoomHeld{ false };
};
