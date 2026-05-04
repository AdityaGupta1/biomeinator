// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include <stb_image_write.h>

#include "rendering/window_manager.h"
#include "rendering/buffer/buffer_helper.h"
#include "settings_manager.h"
#include "logger.h"
#include "util/file_util.h"
#include "util/math.h"

using WindowManager::hwnd;

namespace Renderer
{

void queueScreenshot(const bool useTestOutputPath)
{
    renderState.screenshotRequest.active = true;
    renderState.screenshotRequest.useTestOutputPath = useTestOutputPath;
}

void captureQueuedScreenshot()
{
    RECT rect;
    GetClientRect(hwnd, &rect);
    const uint32_t width = rect.right - rect.left;
    const uint32_t height = rect.bottom - rect.top;

    renderState.screenshotRequest.width = width;
    renderState.screenshotRequest.height = height;

    renderState.screenshotRequest.rowPitchBytes = width * 4;
    renderState.screenshotRequest.rowPitchBytesAligned =
        MathUtil::roundUp(renderState.screenshotRequest.rowPitchBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint32_t readbackSizeBytes = renderState.screenshotRequest.rowPitchBytesAligned * height;

    renderState.screenshotRequest.readbackBuffer = BufferHelper::createBasicBuffer(readbackSizeBytes, &READBACK_HEAP);

    ComPtr<ID3D12Resource> backBuffer;
    CHECK_HRESULT(renderState.proxySwapChain->GetBuffer(renderState.proxySwapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer)));

    D3D12_TEXTURE_COPY_LOCATION srcLocation = {
        .pResource = backBuffer.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };

    D3D12_TEXTURE_COPY_LOCATION destLocation = {};
    destLocation.pResource = renderState.screenshotRequest.readbackBuffer.Get();
    destLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destLocation.PlacedFootprint = {
        .Offset = 0,
        .Footprint = {
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .Width = width,
            .Height = height,
            .Depth = 1,
            .RowPitch = renderState.screenshotRequest.rowPitchBytesAligned,
        },
    };

    BufferHelper::stateTransitionResourceBarrier(
        renderState.cmdList.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    renderState.cmdList->CopyTextureRegion(&destLocation, 0, 0, 0, &srcLocation, nullptr);
    BufferHelper::stateTransitionResourceBarrier(
        renderState.cmdList.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void finalizeQueuedScreenshot()
{
    flush();

    std::vector<uint8_t> pixels(renderState.screenshotRequest.width * renderState.screenshotRequest.height * 4);
    uint8_t* mapped = nullptr;
    renderState.screenshotRequest.readbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (uint32_t row = 0; row < renderState.screenshotRequest.height; ++row)
    {
        memcpy(pixels.data() + renderState.screenshotRequest.rowPitchBytes * row,
               mapped + renderState.screenshotRequest.rowPitchBytesAligned * row,
               renderState.screenshotRequest.rowPitchBytes);
    }
    renderState.screenshotRequest.readbackBuffer->Unmap(0, nullptr);

    std::filesystem::path path;
    if (renderState.screenshotRequest.useTestOutputPath)
    {
        path = std::filesystem::absolute(SettingsManager::getAsString("testOutput"));
        std::filesystem::create_directories(path.parent_path());
    }
    else
    {
        const std::filesystem::path dir = FileUtil::getDocumentsDir("screenshots");
        if (dir.empty())
        {
            throw std::runtime_error("Failed to get screenshots directory");
        }

        const std::string fileName = FileUtil::getTimestampString() + ".png";
        path = dir / fileName;
    }

    stbi_write_png(path.string().c_str(),
                   renderState.screenshotRequest.width,
                   renderState.screenshotRequest.height,
                   4,
                   pixels.data(),
                   renderState.screenshotRequest.width * 4);

    Logger::log("Saved screenshot to %s", path.generic_string().c_str());

    renderState.screenshotRequest = ScreenshotRequest();
}

} // namespace Renderer
