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
*/

#include "window_manager.h"

#include "renderer.h"
#include "settings_manager.h"
#include "scene/gltf_loader.h"

#include <commdlg.h>

#include <locale>
#include <codecvt>

#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace WindowManager
{

HWND hwnd;

bool ignoreOneCursorMovement = true;
bool isCursorVisible = true;
bool isInCursorMode = true;

void setCursorVisibility(bool showCursor)
{
    if (isCursorVisible != showCursor)
    {
        ShowCursor(showCursor);
    }
    isCursorVisible = showCursor;
}

void setIsInCursorMode(bool newIsInCursorMode)
{
    if (newIsInCursorMode)
    {
        isInCursorMode = true;
    }
    else
    {
        ignoreOneCursorMovement = true;
        isInCursorMode = false;
    }

    setCursorVisibility(isInCursorMode);
}

static void onKeyDown(WPARAM wparam)
{
    if (ImGui::GetIO().WantCaptureKeyboard && wparam != VK_ESCAPE)
    {
        return;
    }

    switch (wparam)
    {
        case VK_ESCAPE:
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
                    const std::string filePathStr = converter.to_bytes(std::wstring(filePath, MAX_PATH));
                    // strip hidden characters which otherwise cause issues with file extension comparison
                    const std::string filePathStrClean = std::string(filePathStr.c_str());
                    Renderer::loadGltf(filePathStrClean);
                }
            }
            break;
        case 'P':
            Renderer::queueScreenshot();
            break;
        case 'Z':
            setIsInCursorMode(!isInCursorMode);
            break;
        default:
            break;
    }
}

bool isInitialized = false;

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
            onKeyDown(wparam);
            break;
        case WM_SYSKEYDOWN: // = alt key pressed
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
                setCursorVisibility(true);
            }
            else
            {
                setIsInCursorMode(false);
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            if (isInCursorMode && !ImGui::GetIO().WantCaptureMouse)
            {
                setIsInCursorMode(false);
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

    isInitialized = true;
}

PlayerInput getPlayerInput()
{
    PlayerInput input;

    if (GetForegroundWindow() != WindowManager::hwnd || isInCursorMode)
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

    if (ignoreOneCursorMovement)
    {
        ignoreOneCursorMovement = false;
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
