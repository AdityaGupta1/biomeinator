// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include <fstream>
#include <stb_image_write.h>

#include "logger.h"
#include "rendering/buffer/buffer_helper.h"
#include "rendering/window_manager.h"
#include "settings_manager.h"
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
    auto& request = renderState.screenshotRequest;
    if (request.useTestOutputPath && !SettingsManager::getAsString("testRadianceOutput").empty())
    {
        request.radianceWidth = renderState.renderWidth;
        request.radianceHeight = renderState.renderHeight;
        request.radianceSplits = SettingsManager::getAsBool("doPathSplitting") ? 2 : 1;
        // At the accumulation limit this frame skips tracing and captures the previous
        // result: the counter already equals its sample count, not the last sample index.
        request.radianceDivisor = SettingsManager::getAsUint("antialiasingMode") == 1
                                      ? std::max(1.f, renderState.accumulatedFrameNumber +
                                                           (renderState.stopAccumulating ? 0.f : 1.f))
                                      : 1.f;
        const uint64_t bytes = uint64_t(request.radianceWidth) * request.radianceHeight * request.radianceSplits * 16;
        request.radianceReadback = BufferHelper::createBasicBuffer(bytes, &READBACK_HEAP);
        auto* source = renderState.dev_pathTracingRawBuffer.Get();
        BufferHelper::stateTransitionResourceBarrier(
            renderState.cmdList.Get(), source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        renderState.cmdList->CopyBufferRegion(request.radianceReadback.Get(), 0, source, 0, bytes);
        BufferHelper::stateTransitionResourceBarrier(
            renderState.cmdList.Get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    RECT rect;
    GetClientRect(hwnd, &rect);
    const uint32_t width = rect.right - rect.left;
    const uint32_t height = rect.bottom - rect.top;

    renderState.screenshotRequest.width = width;
    renderState.screenshotRequest.height = height;

    renderState.screenshotRequest.rowPitchBytes = width * 4;
    renderState.screenshotRequest.rowPitchBytesAligned =
        MathUtil::roundUpToPow2(renderState.screenshotRequest.rowPitchBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
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
            .Format = SWAP_CHAIN_FORMAT,
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

    const auto& request = renderState.screenshotRequest;
    if (request.radianceReadback)
    {
        const auto radiancePath = std::filesystem::absolute(SettingsManager::getAsString("testRadianceOutput"));
        std::filesystem::create_directories(radiancePath.parent_path());
        std::ofstream output(radiancePath, std::ios::binary);
        output << "PF\n" << request.radianceWidth << " " << request.radianceHeight << "\n-1.0\n";
        float* raw = nullptr;
        CHECK_HRESULT(request.radianceReadback->Map(0, nullptr, reinterpret_cast<void**>(&raw)));
        for (uint32_t y = request.radianceHeight; y-- > 0;)
            for (uint32_t x = 0; x < request.radianceWidth; ++x)
            {
                float rgb[3]{};
                for (uint32_t split = 0; split < request.radianceSplits; ++split)
                    for (uint32_t c = 0; c < 3; ++c)
                        rgb[c] += raw[((y * request.radianceWidth + x) * request.radianceSplits + split) * 4 + c] /
                                  request.radianceDivisor;
                output.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
            }
        request.radianceReadback->Unmap(0, nullptr);
        if (!output)
            throw std::runtime_error("Failed to write linear radiance image");
        Logger::log("Saved linear radiance to %s", radiancePath.generic_string().c_str());
    }
    renderState.screenshotRequest = ScreenshotRequest();
}

} // namespace Renderer
