/*
* Copyright (c) 2023 NVIDIA CORPORATION. All rights reserved
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#include <array>
#include <mutex>
#include "source/core/sl.log/log.h"
#include "source/core/sl.param/parameters.h"
#include "source/core/sl.security/secureLoadLibrary.h"
#include "vknvll2.h"

namespace sl
{
namespace chi
{

/**
 * Covert SL PCLMarker to Vulkan VkLatencyMarkerNV.
 * @returns value or VK_LATENCY_MARKER_MAX_ENUM_NV on error
*/
VkLatencyMarkerNV PCLMarker2VkLatencyMarker(PCLMarker marker)
{
    switch (marker)
    {
        case PCLMarker::eSimulationStart: return VK_LATENCY_MARKER_SIMULATION_START_NV;
        case PCLMarker::eSimulationEnd: return VK_LATENCY_MARKER_SIMULATION_END_NV;
        case PCLMarker::eRenderSubmitStart: return VK_LATENCY_MARKER_RENDERSUBMIT_START_NV;
        case PCLMarker::eRenderSubmitEnd: return VK_LATENCY_MARKER_RENDERSUBMIT_END_NV;
        case PCLMarker::ePresentStart: return VK_LATENCY_MARKER_PRESENT_START_NV;
        case PCLMarker::ePresentEnd: return VK_LATENCY_MARKER_PRESENT_END_NV;
        //case PCLMarker::eInputSample: return VK_LATENCY_MARKER_INPUT_SAMPLE_NV; // deprecated
        case PCLMarker::eTriggerFlash: return VK_LATENCY_MARKER_TRIGGER_FLASH_NV;

        // Not defined in VK_NV_low_latency2 rev 1
        case PCLMarker::ePCLatencyPing: return VK_LATENCY_MARKER_MAX_ENUM_NV;

        // PCLMarker and corresponding VK_LATENCY_* values differ here on down
        case PCLMarker::eOutOfBandRenderSubmitStart: return VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_START_NV;
        case PCLMarker::eOutOfBandRenderSubmitEnd: return VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_END_NV;
        case PCLMarker::eOutOfBandPresentStart: return VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV;
        case PCLMarker::eOutOfBandPresentEnd: return VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV;

        // Newer markers that are not (yet) in VK_NV_low_latency2 as of rev 1
        case PCLMarker::eControllerInputSample:
        case PCLMarker::eDeltaTCalculation:
        #if defined(SL_REFLEX_LATE_WARP)
        case PCLMarker::eLateWarpPresentStart:
        case PCLMarker::eLateWarpPresentEnd:
        #endif
        case PCLMarker::eMaximum:
        default:
            return VK_LATENCY_MARKER_MAX_ENUM_NV;
    }
}

/**
 * Markers we knowingly can't forward to VK_NV_low_latency2 but that are still legitimate on other
 * backends (D3D/NvAPI). These are expected to be dropped silently on the Vulkan backend. Any other
 * marker that maps to VK_LATENCY_MARKER_MAX_ENUM_NV is treated as unexpected (see setMarker) so a
 * newly added marker that nobody mapped is caught loudly instead of being silently ignored.
 * @returns true if the marker is a known, safe-to-drop gap in the LL2 enum
*/
bool isKnownUnsupportedByLL2(PCLMarker marker)
{
    switch (marker)
    {
        case PCLMarker::ePCLatencyPing:        // telemetry ping, no LL2 slot
        case PCLMarker::eControllerInputSample: // post-dates VK_NV_low_latency2 rev 1
        case PCLMarker::eDeltaTCalculation:     // post-dates VK_NV_low_latency2 rev 1
        #if defined(SL_REFLEX_LATE_WARP)
        case PCLMarker::eLateWarpPresentStart:
        case PCLMarker::eLateWarpPresentEnd:
        #endif
            return true;
        default:
            return false;
    }
}

/**
 * Convert SL OutOfBandCommandQueueType to Vulkan VkOutOfBandQueueTypeNV
 * @returns value or VK_OUT_OF_BAND_QUEUE_TYPE_MAX_ENUM_NV on error
*/
VkOutOfBandQueueTypeNV OOBCmdQueueType2Vk(OutOfBandCommandQueueType type)
{
    switch (type)
    {
        case OutOfBandCommandQueueType::eOutOfBandRender:
            return VK_OUT_OF_BAND_QUEUE_TYPE_RENDER_NV;
        case OutOfBandCommandQueueType::eOutOfBandPresent:
            return VK_OUT_OF_BAND_QUEUE_TYPE_PRESENT_NV;
        default:
            return VK_OUT_OF_BAND_QUEUE_TYPE_MAX_ENUM_NV;
    }
}

class VkNvLowLatency2 : public IReflexVk
{
private:
    VkDevice m_device{};
    VkSwapchainKHR m_swapchain{};

    interposer::VkTable* m_table{};
    VkLayerDispatchTable m_ddt{};

    // VK_NV_low_latency2 entry points resolved locally via vkGetDeviceProcAddr. The interposer
    // does not export these in its dispatch table, so the plugin owns the lookups.
    PFN_vkSetLatencySleepModeNV m_pfnSetLatencySleepModeNV{};
    PFN_vkLatencySleepNV m_pfnLatencySleepNV{};
    PFN_vkSetLatencyMarkerNV m_pfnSetLatencyMarkerNV{};
    PFN_vkGetLatencyTimingsNV m_pfnGetLatencyTimingsNV{};
    PFN_vkQueueNotifyOutOfBandNV m_pfnQueueNotifyOutOfBandNV{};

    VkSemaphore m_semaphore{};
    uint64_t m_semaphoreValue = 0;
    bool m_isLatencyModeEnabled = false;

    // Guards m_swapchain (and m_isLatencyModeEnabled) against the application destroying/recreating
    // the swapchain on one thread while a Reflex call uses it on another. Every LL2 entry point that
    // passes m_swapchain to the driver holds this across that driver call, and create/destroy hold it
    // while mutating the handle - so a destroy can never free a handle that is mid-use. See
    // notifyDestroySwapchain. The blocking semaphore wait in sleep() is deliberately left unlocked.
    std::mutex m_swapchainMtx;

public:
    ComputeStatus init(VkDevice device, param::IParameters* params, interposer::VkTable* table)
    {
        m_device = device;
        m_table = table;
        {
            std::lock_guard lock{table->mutex};
            m_ddt = table->dispatchDeviceMap[m_device];
        }

        m_pfnSetLatencySleepModeNV   = (PFN_vkSetLatencySleepModeNV)  m_table->getDeviceProcAddr(m_device, "vkSetLatencySleepModeNV");
        m_pfnLatencySleepNV          = (PFN_vkLatencySleepNV)         m_table->getDeviceProcAddr(m_device, "vkLatencySleepNV");
        m_pfnSetLatencyMarkerNV      = (PFN_vkSetLatencyMarkerNV)     m_table->getDeviceProcAddr(m_device, "vkSetLatencyMarkerNV");
        m_pfnGetLatencyTimingsNV     = (PFN_vkGetLatencyTimingsNV)    m_table->getDeviceProcAddr(m_device, "vkGetLatencyTimingsNV");
        m_pfnQueueNotifyOutOfBandNV  = (PFN_vkQueueNotifyOutOfBandNV) m_table->getDeviceProcAddr(m_device, "vkQueueNotifyOutOfBandNV");

        // If any LL2 entry point is missing, the extension wasn't enabled. Caller can fall back
        // to the NvAPI-based v1 path.
        if (!m_pfnSetLatencySleepModeNV || !m_pfnLatencySleepNV || !m_pfnSetLatencyMarkerNV ||
            !m_pfnGetLatencyTimingsNV || !m_pfnQueueNotifyOutOfBandNV)
        {
            return ComputeStatus::eNoImplementation;
        }

        const VkSemaphoreTypeCreateInfo sem_type_info {
                VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                nullptr,
                VK_SEMAPHORE_TYPE_TIMELINE,
                m_semaphoreValue};
        const VkSemaphoreCreateInfo info {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sem_type_info};
        VK_CHECK(m_ddt.CreateSemaphore(m_device, &info, nullptr, &m_semaphore));
        return ComputeStatus::eOk;
    }

    virtual ComputeStatus shutdown() override
    {
        if (m_semaphore)
        {
            m_ddt.DestroySemaphore(m_device, m_semaphore, nullptr);
            m_semaphore = VK_NULL_HANDLE;
        }
        return ComputeStatus::eOk;
    }

    virtual ComputeStatus notifyCreateSwapchain(SwapChain chain, bool isLatencyModeEnabled) override
    {
        std::lock_guard<std::mutex> lock(m_swapchainMtx);
        m_swapchain = (VkSwapchainKHR)chain;
        m_isLatencyModeEnabled = isLatencyModeEnabled;
        return ComputeStatus::eOk;
    }

    virtual ComputeStatus notifyDestroySwapchain(SwapChain chain) override
    {
        // Called from the vkDestroySwapchainKHR before-hook, i.e. before the driver frees the handle.
        // Drop our cached handle under the lock: any concurrent LL2 call holds the same lock while
        // using m_swapchain, so once we acquire it no call is mid-flight, and subsequent calls see
        // null and no-op until the next create. Only clear if it is the swapchain we are tracking.
        std::lock_guard<std::mutex> lock(m_swapchainMtx);
        if ((VkSwapchainKHR)chain == m_swapchain)
        {
            m_swapchain = VK_NULL_HANDLE;
            m_isLatencyModeEnabled = false;
        }
        return ComputeStatus::eOk;
    }

    virtual ComputeStatus setSleepMode(const ReflexOptions& consts) override
    {
        const VkLatencySleepModeInfoNV info {
            VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
            nullptr,
            consts.mode != ReflexMode::eOff, // eLowLatencyWithBoost is also "on"
            consts.mode == ReflexMode::eLowLatencyWithBoost,
            consts.frameLimitUs,
        };
        std::lock_guard<std::mutex> lock(m_swapchainMtx);
        if (!m_swapchain)
        {
            return ComputeStatus::eOk;
        }
        VK_CHECK(m_pfnSetLatencySleepModeNV(m_device, m_swapchain, &info));
        return ComputeStatus::eOk;
    }

    virtual ComputeStatus getSleepStatus(ReflexState&) override
    {
        // DX12 (in generic.cpp) and NvLL_VK_* implementations use this method as a "is reflex available/working" call.
        // NOT whether or not it's enabled- they don't set anything in ReflexState.
        if (m_pfnLatencySleepNV)
        {
            return ComputeStatus::eOk;
        }
        else
        {
            return ComputeStatus::eError;
        }
    }
    
    virtual ComputeStatus getReport(ReflexState& settings) override
    {
        std::lock_guard<std::mutex> lock(m_swapchainMtx);
        if (!m_swapchain)
        {
            return ComputeStatus::eOk;
        }
        VkGetLatencyMarkerInfoNV info {
            VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV,
            nullptr,
            0,
            nullptr,
        };
        m_pfnGetLatencyTimingsNV(m_device, m_swapchain, &info);

        std::array<VkLatencyTimingsFrameReportNV, kReflexFrameReportCount> reports{};
        for (auto& r : reports)
        {
            r.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
        }
        info.timingCount = std::min<uint32_t>(info.timingCount, kReflexFrameReportCount);
        info.pTimings = reports.data();
        m_pfnGetLatencyTimingsNV(m_device, m_swapchain, &info);
        
        const auto timing_count = info.timingCount;
        for (uint32_t i = 0; i < kReflexFrameReportCount; ++i)
        {
            if (i >= timing_count)
            {
                settings.frameReport[i] = ReflexReport();
                continue;
            }
            settings.frameReport[i].frameID = reports[i].presentID;
            settings.frameReport[i].inputSampleTime = reports[i].inputSampleTimeUs;
            settings.frameReport[i].simStartTime = reports[i].simStartTimeUs;
            settings.frameReport[i].simEndTime = reports[i].simEndTimeUs;
            settings.frameReport[i].renderSubmitStartTime = reports[i].renderSubmitStartTimeUs;
            settings.frameReport[i].renderSubmitEndTime = reports[i].renderSubmitEndTimeUs;
            settings.frameReport[i].presentStartTime = reports[i].presentStartTimeUs;
            settings.frameReport[i].presentEndTime = reports[i].presentEndTimeUs;
            settings.frameReport[i].driverStartTime = reports[i].driverStartTimeUs;
            settings.frameReport[i].driverEndTime = reports[i].driverEndTimeUs;
            settings.frameReport[i].osRenderQueueStartTime = reports[i].osRenderQueueStartTimeUs;
            settings.frameReport[i].osRenderQueueEndTime = reports[i].osRenderQueueEndTimeUs;
            settings.frameReport[i].gpuRenderStartTime = reports[i].gpuRenderStartTimeUs;
            settings.frameReport[i].gpuRenderEndTime = reports[i].gpuRenderEndTimeUs;
            settings.frameReport[i].gpuActiveRenderTimeUs = (uint32_t)(reports[i].gpuRenderEndTimeUs - reports[i].gpuRenderStartTimeUs);
            settings.frameReport[i].gpuFrameTimeUs = i == 0 ? 0 : (uint32_t)(reports[i].gpuRenderEndTimeUs - reports[i - 1].gpuRenderEndTimeUs);
        }
        return ComputeStatus::eOk;
    }

    virtual ComputeStatus sleep() override
    {
        {
            std::lock_guard<std::mutex> lock(m_swapchainMtx);
            if (!m_swapchain)
            {
                // Swapchain destroyed (or not yet created) - nothing to sleep on.
                return ComputeStatus::eOk;
            }
            // If the swapchain doesn't have latency-mode enabled WaitSemaphores will just timeout
            if (!m_isLatencyModeEnabled)
            {
                SL_LOG_WARN("Reflex sleep called, but swapchain doesn't have latency-mode enabled");
                return ComputeStatus::eOk;
            }

            m_semaphoreValue++;
            const VkLatencySleepInfoNV info {
                VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV,
                nullptr,
                m_semaphore,
                m_semaphoreValue,
            };
            // vkLatencySleepNV consumes the swapchain, so issue it under the lock. The wait below only
            // touches our own device-lifetime timeline semaphore (not the swapchain), so it runs
            // unlocked - holding the lock across a full frame's sleep would stall create/destroy.
            VK_CHECK(m_pfnLatencySleepNV(m_device, m_swapchain, &info));
        }

        const VkSemaphoreWaitInfo waitInfo {
            VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            nullptr,
            0, // flags
            1, // semaphoreCount
            &m_semaphore,
            &m_semaphoreValue,
        };
        const VkResult res = m_ddt.WaitSemaphores(m_device, &waitInfo, kMaxSemaphoreWaitUs);
        if (res < 0)
        {
            SL_LOG_ERROR("WaitSemaphores: %d", res);
            return ComputeStatus::eError;
        }
        else if (res != VK_SUCCESS)
        {
            SL_LOG_WARN("WaitSemaphores %d", res);
        }
        return ComputeStatus::eOk;
    }

    virtual ComputeStatus setMarker(PCLMarker marker, uint64_t frameId) override
    {
        const VkLatencyMarkerNV vk_marker = PCLMarker2VkLatencyMarker(marker);
        if (vk_marker == VK_LATENCY_MARKER_MAX_ENUM_NV)
        {
            if (isKnownUnsupportedByLL2(marker))
            {
                // Known gap: this marker has no VK_NV_low_latency2 equivalent, but is legitimately
                // emitted every frame and consumed on other backends (D3D/NvAPI). Skip quietly and
                // return eOk rather than erroring every frame, which otherwise spammed the log
                // (bug 6463049).
                SL_LOG_WARN_ONCE("PCL marker %d has no VK_NV_low_latency2 (LL2) equivalent; skipping "
                    "on the Vulkan backend (may still be valid on other backends, e.g. D3D/NvAPI)",
                    static_cast<int>(marker));
            }
            else
            {
                // Unexpected: a marker with no LL2 mapping that isn't on the known-unsupported list -
                // most likely a newly added marker nobody wired into PCLMarker2VkLatencyMarker. Make
                // it visible (once, so it can't re-spam the log) and trip a dev-build assert; still
                // return eOk so the caller's per-frame CHI_VALIDATE doesn't reopen bug 6463049.
                SL_LOG_ERROR_ONCE("PCL marker %d is unknown to the VK LL2 backend and has no mapping; "
                    "add it to PCLMarker2VkLatencyMarker or isKnownUnsupportedByLL2",
                    static_cast<int>(marker));
                assert(false);
            }
            return ComputeStatus::eOk;
        }
        const VkSetLatencyMarkerInfoNV info {
            VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV,
            nullptr,
            frameId,
            vk_marker,
        };
        std::lock_guard<std::mutex> lock(m_swapchainMtx);
        if (!m_swapchain)
        {
            return ComputeStatus::eOk;
        }
        m_pfnSetLatencyMarkerNV(m_device, m_swapchain, &info);
        return ComputeStatus::eOk;
    }

    virtual bool supportsLatencySubmissionPresentId() const override { return true; }

    virtual ComputeStatus notifyOutOfBandCommandQueue(CommandQueue queue, OutOfBandCommandQueueType type) override
    {
        const VkOutOfBandQueueTypeInfoNV info {
            VK_STRUCTURE_TYPE_OUT_OF_BAND_QUEUE_TYPE_INFO_NV,
            nullptr,
            OOBCmdQueueType2Vk(type)
        };
        if (info.queueType == VK_OUT_OF_BAND_QUEUE_TYPE_MAX_ENUM_NV)
        {
            SL_LOG_WARN("Unknown OOB queue type: %d", type);
            return ComputeStatus::eInvalidArgument;
        }
        auto cmd_q = (CommandQueueVk*)queue;
        assert(cmd_q->type == ResourceType::eCommandQueue);
        auto vk_queue = (VkQueue)cmd_q->native;
        m_pfnQueueNotifyOutOfBandNV(vk_queue, &info);
        return ComputeStatus::eOk;
    }

};

IReflexVk* CreateVkNvLowLatency2(VkDevice device, param::IParameters* params, interposer::VkTable* table)
{
    auto ptr = new VkNvLowLatency2();
    ComputeStatus res = ptr->init(device, params, table);
    if (res != ComputeStatus::eOk)
    {
        SL_LOG_INFO("Failed to init VkNvLowLatency2: %d", res);
        delete ptr;
        ptr = nullptr;
    }
    return ptr;
}

} // namespace chi
} // namespace sl
