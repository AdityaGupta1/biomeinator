#include "settings_manager.h"

#include "rendering/renderer.h"
#include "rendering/window_manager.h"

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
