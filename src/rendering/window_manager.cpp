// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "window_manager.h"

#include "renderer.h"
#include "settings_manager.h"
#include "scene/gltf_loader.h"

#include <commdlg.h>
#include <locale>
#include <codecvt>

#include <hidsdi.h>
#include <vector>

#include "logger.h"

#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace WindowManager
{

HWND hwnd;

static bool isCursorVisible = true;
static bool isInCursorMode = true;

static void setCursorVisibility(bool showCursor)
{
    if (isCursorVisible != showCursor)
    {
        ShowCursor(showCursor);
    }
    isCursorVisible = showCursor;
}

static int mouseRawDx = 0;
static int mouseRawDy = 0;

static void resetMouseRawDeltas()
{
    mouseRawDx = mouseRawDy = 0;
}

static void setIsInCursorMode(bool newIsInCursorMode)
{
    if (newIsInCursorMode)
    {
        isInCursorMode = true;
    }
    else
    {
        isInCursorMode = false;
        resetMouseRawDeltas();
    }

    setCursorVisibility(isInCursorMode);
}

static WINDOWPLACEMENT prevWindowPlacement;

void enterFullscreen()
{
    prevWindowPlacement.length = sizeof(prevWindowPlacement);
    GetWindowPlacement(hwnd, &prevWindowPlacement);

    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);

    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    style &= ~(WS_OVERLAPPEDWINDOW);
    SetWindowLong(hwnd, GWL_STYLE, style);

    SetWindowPos(hwnd,
                 HWND_TOP,
                 mi.rcMonitor.left,
                 mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}

void exitFullscreen()
{
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    style |= WS_OVERLAPPEDWINDOW;
    SetWindowLong(hwnd, GWL_STYLE, style);

    SetWindowPlacement(hwnd, &prevWindowPlacement);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

void toggleFullscreen()
{
    const bool isFullscreen = SettingsManager::getAsBool("fullscreen");

    if (isFullscreen)
    {
        exitFullscreen();
    }
    else
    {
        enterFullscreen();
    }

    SettingsManager::setAsBool("fullscreen", !isFullscreen);
}

static void onKeyDown(WPARAM wparam, LPARAM lparam)
{
    const bool isRepeat = (lparam & (1u << 30)) != 0;
    if (isRepeat)
    {
        return;
    }

    if (ImGui::GetIO().WantCaptureKeyboard && wparam != VK_ESCAPE)
    {
        return;
    }

    const bool ctrlHeld = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool altHeld = (GetKeyState(VK_MENU) & 0x8000) != 0;

    switch (wparam)
    {
        case VK_ESCAPE:
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            break;
        case 'H':
            SettingsManager::toggleBool("showGui");
            break;
        case 'O':
            if (ctrlHeld && !SettingsManager::getAsBool("voxelMode"))
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
                    const std::string filePathStr = converter.to_bytes(std::wstring(filePath, MAX_PATH));
                    // strip hidden characters which otherwise cause issues with file extension comparison
                    const std::string filePathStrClean = std::string(filePathStr.c_str());
                    Renderer::loadScene(filePathStrClean);
                }
            }
            break;
        case 'P':
            Renderer::queueScreenshot();
            break;
        case 'Z':
            setIsInCursorMode(!isInCursorMode);
            break;
        case VK_RETURN:
            if (!altHeld)
            {
                break;
            }
            [[fallthrough]];
        case VK_F11:
            toggleFullscreen();
            break;
        default:
            break;
    }
}

static void setMouseDeltas(const LPARAM lparam)
{
    UINT byteSize = 0;
    GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr, &byteSize, sizeof(RAWINPUTHEADER));
    if (byteSize == 0)
    {
        return;
    }

    std::vector<BYTE> buf(byteSize);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, buf.data(), &byteSize, sizeof(RAWINPUTHEADER)) !=
        byteSize)
    {
        return;
    }

    RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(buf.data());
    if (ri->header.dwType == RIM_TYPEMOUSE)
    {
        mouseRawDx += static_cast<int>(ri->data.mouse.lLastX);
        mouseRawDy += static_cast<int>(ri->data.mouse.lLastY);
    }
}

static bool isInitialized = false;

static LRESULT WINAPI onWindowMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
    {
        return true;
    }

    switch (msg)
    {
        case WM_CLOSE:
        case WM_DESTROY:
            Renderer::destroy();
            PostQuitMessage(0);
            break;
        case WM_SIZING:
        case WM_SIZE:
            if (isInitialized)
            {
                Renderer::resize();
            }
            break;
        case WM_KEYDOWN:
            onKeyDown(wparam, lparam);
            return 0;
        case WM_SYSKEYDOWN: // = alt key pressed
            if (wparam == VK_F4)
            {
                break;
            }
            onKeyDown(wparam, lparam);
            return 0;
        case WM_SYSCHAR: // = default Windows alt menus, etc.
            return 0;
        case WM_ACTIVATE:
            if (wparam == WA_INACTIVE)
            {
                setCursorVisibility(true);
            }
            else
            {
                const bool didClickOnGui = ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
                if (!didClickOnGui)
                {
                    setIsInCursorMode(false);
                }
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            if (isInCursorMode && !ImGui::GetIO().WantCaptureMouse)
            {
                setIsInCursorMode(false);
            }
            break;
        case WM_INPUT:
        {
            setMouseDeltas(lparam);
            break;
        }
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
        .lpszClassName = L"BiomeinatorClass",
    };
    RegisterClassW(&wcw);

    RECT rect;
    rect.left = 0;
    rect.top = 0;
    rect.right = SettingsManager::getAsUint("width");
    rect.bottom = SettingsManager::getAsUint("height");
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE); // account for window header bar

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    hwnd = CreateWindowExW(0,
                           L"BiomeinatorClass",
                           L"Biomeinator",
                           WS_VISIBLE | WS_OVERLAPPEDWINDOW,
                           320,
                           180,
                           width,
                           height,
                           nullptr,
                           nullptr,
                           nullptr,
                           nullptr);

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; // generic desktop controls
    rid.usUsage = 0x02; // mouse
    rid.dwFlags = 0; // receive when focused; keep legacy messages for ImGui
    rid.hwndTarget = hwnd;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
    {
        Logger::logWarning("Failed to register raw input devices");
    }

    isInitialized = true;

    if (SettingsManager::getAsBool("fullscreen"))
    {
        prevWindowPlacement = { sizeof(prevWindowPlacement) };
        enterFullscreen();
    }
}

PlayerInput getPlayerInput()
{
    PlayerInput input;

    if (isInCursorMode || GetForegroundWindow() != WindowManager::hwnd)
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

    if (input.linearInput.x != 0 || input.linearInput.z != 0)
    {
        const float inputHorizontalLength =
            sqrt(input.linearInput.x * input.linearInput.x + input.linearInput.z * input.linearInput.z);
        input.linearInput.x /= inputHorizontalLength;
        input.linearInput.z /= inputHorizontalLength;
    }

    input.mouseMovement.x = mouseRawDx;
    input.mouseMovement.y = mouseRawDy;
    resetMouseRawDeltas();

    RECT windowRect;
    GetWindowRect(WindowManager::hwnd, &windowRect);
    const int centerX = (windowRect.left + windowRect.right) / 2;
    const int centerY = (windowRect.top + windowRect.bottom) / 2;
    SetCursorPos(centerX, centerY);

    return input;
}

}; // namespace WindowManager
