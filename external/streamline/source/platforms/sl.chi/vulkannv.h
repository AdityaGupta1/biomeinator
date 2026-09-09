// This file is for API functionality that isn't yet in a shipping SDK

#ifndef _VULKANNV_H
#define _VULKANNV_H 1

#define VK_NO_PROTOTYPES 1

#ifndef VK_NV_low_latency2

#define VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV ((VkStructureType)1000505000)
#define VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV ((VkStructureType)1000505001)
#define VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV ((VkStructureType)1000505002)
#define VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV ((VkStructureType)1000505003)
#define VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV ((VkStructureType)1000505004)
#define VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV ((VkStructureType)1000505005)
#define VK_STRUCTURE_TYPE_OUT_OF_BAND_QUEUE_TYPE_INFO_NV ((VkStructureType)1000505006)
#define VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV ((VkStructureType)1000505007)
#define VK_STRUCTURE_TYPE_LATENCY_SURFACE_CAPABILITIES_NV ((VkStructureType)1000505008)

// VK_NV_low_latency2 is a preprocessor guard. Do not pass it to API calls.
#define VK_NV_low_latency2 1
#define VK_NV_LOW_LATENCY_2_SPEC_VERSION  2
#define VK_NV_LOW_LATENCY_2_EXTENSION_NAME "VK_NV_low_latency2"

typedef enum VkLatencyMarkerNV {
    VK_LATENCY_MARKER_SIMULATION_START_NV = 0,
    VK_LATENCY_MARKER_SIMULATION_END_NV = 1,
    VK_LATENCY_MARKER_RENDERSUBMIT_START_NV = 2,
    VK_LATENCY_MARKER_RENDERSUBMIT_END_NV = 3,
    VK_LATENCY_MARKER_PRESENT_START_NV = 4,
    VK_LATENCY_MARKER_PRESENT_END_NV = 5,
    VK_LATENCY_MARKER_INPUT_SAMPLE_NV = 6,
    VK_LATENCY_MARKER_TRIGGER_FLASH_NV = 7,
    VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_START_NV = 8,
    VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_END_NV = 9,
    VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV = 10,
    VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV = 11,
    VK_LATENCY_MARKER_MAX_ENUM_NV = 0x7FFFFFFF
} VkLatencyMarkerNV;

typedef enum VkOutOfBandQueueTypeNV {
    VK_OUT_OF_BAND_QUEUE_TYPE_RENDER_NV = 0,
    VK_OUT_OF_BAND_QUEUE_TYPE_PRESENT_NV = 1,
    VK_OUT_OF_BAND_QUEUE_TYPE_MAX_ENUM_NV = 0x7FFFFFFF
} VkOutOfBandQueueTypeNV;
typedef struct VkLatencySleepModeInfoNV {
    VkStructureType    sType;
    const void*        pNext;
    VkBool32           lowLatencyMode;
    VkBool32           lowLatencyBoost;
    uint32_t           minimumIntervalUs;
} VkLatencySleepModeInfoNV;

typedef struct VkLatencySleepInfoNV {
    VkStructureType    sType;
    const void*        pNext;
    VkSemaphore        signalSemaphore;
    uint64_t           value;
} VkLatencySleepInfoNV;

typedef struct VkSetLatencyMarkerInfoNV {
    VkStructureType      sType;
    const void*          pNext;
    uint64_t             presentID;
    VkLatencyMarkerNV    marker;
} VkSetLatencyMarkerInfoNV;

typedef struct VkLatencyTimingsFrameReportNV {
    VkStructureType    sType;
    const void*        pNext;
    uint64_t           presentID;
    uint64_t           inputSampleTimeUs;
    uint64_t           simStartTimeUs;
    uint64_t           simEndTimeUs;
    uint64_t           renderSubmitStartTimeUs;
    uint64_t           renderSubmitEndTimeUs;
    uint64_t           presentStartTimeUs;
    uint64_t           presentEndTimeUs;
    uint64_t           driverStartTimeUs;
    uint64_t           driverEndTimeUs;
    uint64_t           osRenderQueueStartTimeUs;
    uint64_t           osRenderQueueEndTimeUs;
    uint64_t           gpuRenderStartTimeUs;
    uint64_t           gpuRenderEndTimeUs;
} VkLatencyTimingsFrameReportNV;

typedef struct VkGetLatencyMarkerInfoNV {
    VkStructureType                   sType;
    const void*                       pNext;
    uint32_t                          timingCount;
    VkLatencyTimingsFrameReportNV*    pTimings;
} VkGetLatencyMarkerInfoNV;

typedef struct VkLatencySubmissionPresentIdNV {
    VkStructureType    sType;
    const void*        pNext;
    uint64_t           presentID;
} VkLatencySubmissionPresentIdNV;

typedef struct VkSwapchainLatencyCreateInfoNV {
    VkStructureType    sType;
    const void*        pNext;
    VkBool32           latencyModeEnable;
} VkSwapchainLatencyCreateInfoNV;

typedef struct VkOutOfBandQueueTypeInfoNV {
    VkStructureType           sType;
    const void*               pNext;
    VkOutOfBandQueueTypeNV    queueType;
} VkOutOfBandQueueTypeInfoNV;

typedef struct VkLatencySurfaceCapabilitiesNV {
    VkStructureType      sType;
    const void*          pNext;
    uint32_t             presentModeCount;
    VkPresentModeKHR*    pPresentModes;
} VkLatencySurfaceCapabilitiesNV;

typedef VkResult (VKAPI_PTR *PFN_vkSetLatencySleepModeNV)(VkDevice device, VkSwapchainKHR swapchain, const VkLatencySleepModeInfoNV* pSleepModeInfo);
typedef VkResult (VKAPI_PTR *PFN_vkLatencySleepNV)(VkDevice device, VkSwapchainKHR swapchain, const VkLatencySleepInfoNV* pSleepInfo);
typedef void (VKAPI_PTR *PFN_vkSetLatencyMarkerNV)(VkDevice device, VkSwapchainKHR swapchain, const VkSetLatencyMarkerInfoNV* pLatencyMarkerInfo);
typedef void (VKAPI_PTR *PFN_vkGetLatencyTimingsNV)(VkDevice device, VkSwapchainKHR swapchain, VkGetLatencyMarkerInfoNV* pLatencyMarkerInfo);
typedef void (VKAPI_PTR *PFN_vkQueueNotifyOutOfBandNV)(VkQueue queue, const VkOutOfBandQueueTypeInfoNV* pQueueTypeInfo);

#ifndef VK_NO_PROTOTYPES
VKAPI_ATTR VkResult VKAPI_CALL vkSetLatencySleepModeNV(
    VkDevice                                    device,
    VkSwapchainKHR                              swapchain,
    const VkLatencySleepModeInfoNV*             pSleepModeInfo);

VKAPI_ATTR VkResult VKAPI_CALL vkLatencySleepNV(
    VkDevice                                    device,
    VkSwapchainKHR                              swapchain,
    const VkLatencySleepInfoNV*                 pSleepInfo);

VKAPI_ATTR void VKAPI_CALL vkSetLatencyMarkerNV(
    VkDevice                                    device,
    VkSwapchainKHR                              swapchain,
    const VkSetLatencyMarkerInfoNV*             pLatencyMarkerInfo);

VKAPI_ATTR void VKAPI_CALL vkGetLatencyTimingsNV(
    VkDevice                                    device,
    VkSwapchainKHR                              swapchain,
    VkGetLatencyMarkerInfoNV*                   pLatencyMarkerInfo);

VKAPI_ATTR void VKAPI_CALL vkQueueNotifyOutOfBandNV(
    VkQueue                                     queue,
    const VkOutOfBandQueueTypeInfoNV*           pQueueTypeInfo);
#endif

#endif //ndef VK_NV_low_latency2

#endif  //ndef _VULKANNV_H
