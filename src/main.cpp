#include "settings_manager.h"

#include "rendering/renderer.h"
#include "rendering/window_manager.h"

int main(int argc, char** argv)
{
    SettingsManager::parseArgs(argc, argv);

    WindowManager::init();
    Renderer::init();

    const bool hasTestOutput = SettingsManager::hasOption("test-output");
    if (hasTestOutput)
    {
        const std::string path = SettingsManager::getAsString("test-output");
        Renderer::queueScreenshot(path);
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
