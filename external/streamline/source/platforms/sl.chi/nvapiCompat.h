/*
 * Copyright (c) 2023-2025 NVIDIA CORPORATION. All rights reserved
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

#pragma once

#include "nvapi.h"

//TODO: Remove these binary compatible structs once we update the NVAPI headers.

typedef struct
{
    NvU32  version;
    NvBool bLowLatencyMode;
    NvBool bFsVrr;
    NvBool bCplVsyncOn;
    NvU32  sleepIntervalUs;
    NvBool bUseGameSleep;
    NvBool bFullscreenIFlip;
    NvU8   fgMultiplier;
    NvBool bDfgControl;
    NvU32  dfgFrameTimeTargetUs;
    NvU8   rsvd[114];
} NV_GET_SLEEP_STATUS_PARAMS_V1_BFM_37870595;

typedef struct
{
    NvU32  version;
    NvU32  bEnable:1;
    NvU32  bDisable:1;
    NvU32  flagsRsvd:30;
    NvU32  vblankIntervalUs;
    NvS32  timeInQueueUs;
    NvU32  timeInQueueUsTarget;
    NvU8   fgMultiplier;
    NvU8   dfgMaxMultiplier;
    NvU8   rsvd2[2];
    NvU32  dfgTargetFps;
    NvU8   rsvd[20];
} NV_SET_REFLEX_SYNC_PARAMS_V1_BFM_37843738;
