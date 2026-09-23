/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */
#pragma once

#define CFG_TUSB_MCU OPT_MCU_NONE
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUD_ENABLED 1
#define CFG_TUD_HID 4
#define CFG_TUD_MSC 1
#define CFG_TUD_HID_EP_BUFSIZE 64
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_MSC_EP_BUFSIZE 512
#define TUP_DCD_ENDPOINT_MAX 8
