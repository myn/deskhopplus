/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#pragma once
/*
 * The icon resources deskhop-helper.rc embeds (#208), by id. Plain defines
 * because the resource compiler reads this file too.
 *
 * IDI_APP is 1 and stays the lowest: Explorer shows an exe's first icon group,
 * and that is the tile, not a tray glyph.
 */
#define IDI_APP 1
#define IDI_PAIRED 2
#define IDI_OFF 3
#define IDI_ATTENTION 4
