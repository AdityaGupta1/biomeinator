#include "window_manager.h"

#include "renderer.h"

namespace WindowManager
{

HWND hwnd;

bool didWindowJustRegainFocus = true;

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
        if (wparam == VK_ESCAPE)
        {
            Renderer::flush();
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
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

    WNDCLASSW wcw = { .lpfnWndProc = &onWindowMessage,
                      .hCursor = LoadCursor(nullptr, IDC_ARROW),
                      .lpszClassName = L"GigaMinecraftClass" };
    RegisterClassW(&wcw);

    hwnd = CreateWindowExW(0,
                           L"GigaMinecraftClass",
                           L"Giga Minecraft",
                           WS_VISIBLE | WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT,
                           CW_USEDEFAULT,
                           /*width=*/CW_USEDEFAULT,
                           /*height=*/CW_USEDEFAULT,
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
    int centerX = (windowRect.left + windowRect.right) / 2;
    int centerY = (windowRect.top + windowRect.bottom) / 2;

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
