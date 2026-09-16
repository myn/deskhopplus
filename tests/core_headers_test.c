/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include "dh_helper.h"
#include "dh_inq.h"
#include "dh_keymap.h"

DH_STATIC_ASSERT(sizeof(uint8_t) == 1, "one-byte wire unit");
