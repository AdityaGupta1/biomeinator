#pragma once

#include "dxr_includes.h"

struct PlayerInput
{
    DirectX::XMFLOAT3 linearInput{ 0, 0, 0 };
    float linearSpeedMultiplier{ 1.f };
    DirectX::XMFLOAT2 mouseMovement{ 0, 0 };
};

namespace WindowManager
{

	extern HWND hwnd;

    void init();

    PlayerInput getPlayerInput();

}; // namespace WindowManager
