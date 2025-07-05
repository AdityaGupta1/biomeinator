#include "window_manager.h"

#include "renderer.h"
#include "scene/gltf_loader.h"

#include <commdlg.h>

#include <locale>
#include <codecvt>

namespace WindowManager
{

HWND hwnd;

bool didWindowJustRegainFocus = true;

static void onKeyDown(WPARAM wparam)
{
    switch (wparam)
    {
    case VK_ESCAPE:
        Renderer::flush();
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        break;
    case 'O':
        if (GetKeyState(VK_CONTROL) & 0x8000)
        {
            OPENFILENAMEW ofn{};
            wchar_t filePath[MAX_PATH] = L"";

            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"glTF Files (*.gltf; *.glb)\0*.gltf;*.glb\0";
            ofn.lpstrTitle = L"Open glTF file";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

            if (GetOpenFileNameW(&ofn))
            {
                std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
                const std::string filePathStr =
                    converter.to_bytes(std::wstring(filePath, MAX_PATH));
                // strip hidden characters which otherwise cause issues with file extension comparison
                const std::string filePathStrClean = std::string(filePathStr.c_str());
                Renderer::loadGltf(filePathStrClean);
            }
        }
        break;
    case 'P':
        Renderer::saveScreenshot();
        break;
    default:
        break;
    }
}

LRESULT WINAPI onWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CLOSE:
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_SIZING:
    case WM_SIZE:
        Renderer::resize();
        break;
    case WM_KEYDOWN:
        onKeyDown(wparam);
        break;
    case WM_SYSKEYDOWN:      // = alt key pressed
        if (wparam == VK_F4) // allow alt + f4
        {
            break;
        }
        [[fallthrough]];
    case WM_SYSKEYUP:
    case WM_SYSCHAR: // = key pressed while alt is also pressed
        return 0;
    case WM_ACTIVATE:
        if (wparam == WA_INACTIVE)
        {
            ShowCursor(true);
        }
        else
        {
            didWindowJustRegainFocus = true;
            ShowCursor(false);
        }
        break;
    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void init()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wcw = {
        .lpfnWndProc = &onWindowMessage,
        .hCursor = LoadCursor(nullptr, IDC_ARROW),
        .lpszClassName = L"GigaMinecraftClass",
    };
    RegisterClassW(&wcw);

    hwnd = CreateWindowExW(0,
                           L"GigaMinecraftClass",
                           L"Giga Minecraft",
                           WS_VISIBLE | WS_OVERLAPPEDWINDOW,
                           320,
                           180,
                           1920,
                           1080,
                           nullptr,
                           nullptr,
                           nullptr,
                           nullptr);
}

PlayerInput getPlayerInput()
{
    PlayerInput input;

    if (GetForegroundWindow() != WindowManager::hwnd)
    {
        return input;
    }

#define KEY_DOWN(key) (GetAsyncKeyState(key) & 0x8000)

    if (KEY_DOWN('W'))
    {
        ++input.linearInput.z;
    }

    if (KEY_DOWN('A'))
    {
        --input.linearInput.x;
    }

    if (KEY_DOWN('S'))
    {
        --input.linearInput.z;
    }

    if (KEY_DOWN('D'))
    {
        ++input.linearInput.x;
    }

    if (KEY_DOWN(VK_SPACE) || KEY_DOWN('E'))
    {
        ++input.linearInput.y;
    }

    if (KEY_DOWN('Q'))
    {
        --input.linearInput.y;
    }

    if (KEY_DOWN(VK_LSHIFT))
    {
        input.linearSpeedMultiplier *= 2.f;
    }

    if (KEY_DOWN(VK_LMENU))
    {
        input.linearSpeedMultiplier *= 0.5f;
    }

    input.isZoomHeld = KEY_DOWN('C');

#undef KEY_DOWN

    POINT cursorPos;
    GetCursorPos(&cursorPos);

    RECT windowRect;
    GetWindowRect(WindowManager::hwnd, &windowRect);
    const int centerX = (windowRect.left + windowRect.right) / 2;
    const int centerY = (windowRect.top + windowRect.bottom) / 2;

    if (didWindowJustRegainFocus)
    {
        didWindowJustRegainFocus = false;
    }
    else
    {
        input.mouseMovement.x = static_cast<float>(cursorPos.x - centerX);
        input.mouseMovement.y = static_cast<float>(cursorPos.y - centerY);
    }

    SetCursorPos(centerX, centerY);

    return input;
}

}; // namespace WindowManager
