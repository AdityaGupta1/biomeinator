// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include <shlobj.h>

#include <filesystem>

#include <stb_image_write.h>

#include "rendering/window_manager.h"
#include "rendering/buffer/buffer_helper.h"
#include "settings_manager.h"
#include "logger.h"
#include "util/math.h"

using WindowManager::hwnd;

namespace Renderer
{

void queueScreenshot(const bool useTestOutputPath)
{
    screenshotRequest.active = true;
    screenshotRequest.useTestOutputPath = useTestOutputPath;
}

void captureQueuedScreenshot()
{
    RECT rect;
    GetClientRect(hwnd, &rect);
    const uint32_t width = rect.right - rect.left;
    const uint32_t height = rect.bottom - rect.top;

    screenshotRequest.width = width;
    screenshotRequest.height = height;

    screenshotRequest.rowPitchBytes = width * 4;
    screenshotRequest.rowPitchBytesAligned =
        MathUtil::roundUp(screenshotRequest.rowPitchBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint32_t readbackSizeBytes = screenshotRequest.rowPitchBytesAligned * height;

    screenshotRequest.readbackBuffer = BufferHelper::createBasicBuffer(readbackSizeBytes, &READBACK_HEAP);

    ComPtr<ID3D12Resource> backBuffer;
    CHECK_HRESULT(swapChain->GetBuffer(swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer)));

    D3D12_TEXTURE_COPY_LOCATION srcLocation = {
        .pResource = backBuffer.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };

    D3D12_TEXTURE_COPY_LOCATION destLocation = {};
    destLocation.pResource = screenshotRequest.readbackBuffer.Get();
    destLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destLocation.PlacedFootprint = {
        .Offset = 0,
        .Footprint = {
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .Width = width,
            .Height = height,
            .Depth = 1,
            .RowPitch = screenshotRequest.rowPitchBytesAligned,
        },
    };

    BufferHelper::stateTransitionResourceBarrier(
        cmdList.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&destLocation, 0, 0, 0, &srcLocation, nullptr);
    BufferHelper::stateTransitionResourceBarrier(
        cmdList.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void finalizeQueuedScreenshot()
{
    flush();

    std::vector<uint8_t> pixels(screenshotRequest.width * screenshotRequest.height * 4);
    uint8_t* mapped = nullptr;
    screenshotRequest.readbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (uint32_t row = 0; row < screenshotRequest.height; ++row)
    {
        memcpy(pixels.data() + screenshotRequest.rowPitchBytes * row,
               mapped + screenshotRequest.rowPitchBytesAligned * row,
               screenshotRequest.rowPitchBytes);
    }
    screenshotRequest.readbackBuffer->Unmap(0, nullptr);

    std::filesystem::path path;
    if (screenshotRequest.useTestOutputPath)
    {
        path = std::filesystem::absolute(SettingsManager::getAsString("testOutput"));
    }
    else
    {
        wchar_t documentsPath[MAX_PATH];
        if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documentsPath)))
        {
            throw std::runtime_error("Failed to get screenshots directory");
        }

        const std::filesystem::path dir = std::filesystem::path(documentsPath) / "biomeinator" / "screenshots";

        SYSTEMTIME st{};
        GetLocalTime(&st);
        char fileName[64];
        sprintf_s(fileName,
                  "%04d.%02d.%02d_%02d-%02d-%02d.png",
                  st.wYear,
                  st.wMonth,
                  st.wDay,
                  st.wHour,
                  st.wMinute,
                  st.wSecond);

        path = dir / fileName;
    }

    std::filesystem::create_directories(path.parent_path());

    stbi_write_png(path.string().c_str(),
                   screenshotRequest.width,
                   screenshotRequest.height,
                   4,
                   pixels.data(),
                   screenshotRequest.width * 4);

    Logger::log("Saved screenshot to %s", path.generic_string().c_str());

    screenshotRequest = ScreenshotRequest();
}

} // namespace Renderer
