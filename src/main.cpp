// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "settings_manager.h"

#include "rendering/renderer.h"
#include "rendering/window_manager.h"

// Agility SDK
extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = 616;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

int main(int argc, char** argv)
{
    SettingsManager::parseArgs(argc, argv);

    WindowManager::init();
    Renderer::init();

    for (MSG msg;;)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                return 0;
            }

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        Renderer::render();
    }
}
