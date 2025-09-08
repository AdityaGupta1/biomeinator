/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/                                                                                                                     \

#include "settings_manager.h"

#include "rendering/renderer.h"
#include "rendering/window_manager.h"

int main(int argc, char** argv)
{
    SettingsManager::parseArgs(argc, argv);

    WindowManager::init();
    Renderer::init();

    if (SettingsManager::getAsString("testOutput") != "")
    {
        const std::string path = SettingsManager::getAsString("testOutput");
        Renderer::queueScreenshot(true /*useTestOutputPath*/);
        Renderer::render();
        Renderer::flush();
        return 0;
    }

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
