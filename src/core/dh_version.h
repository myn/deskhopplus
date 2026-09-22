/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#pragma once

/*
 * The one release version, shown wherever the user can look (#199): the
 * firmware's build readout and config page, and the greyed first row of both
 * helpers' menus, and the Windows exe's own properties (#237). The firmware
 * build parses this file (CMakeLists.txt); the helpers compile it, and the
 * Windows helper's .rc includes it.
 *
 * Move it once per release, for all three, and the four tests with it (the
 * two menu tests, helpers/windows/tests/version_info_test.cpp and
 * tools/macos-checks/app-bundle-tests.sh): they carry the literal on purpose.
 * Only ever upwards: board B pulls firmware from board A on a newer version,
 * or on an equal version carrying a different image, and never on an older
 * one (fw_upgrade.h). Started at 1.0 rather than reset to 0.1 for exactly
 * that reason (#21).
 */
#define DH_VERSION_MAJOR 1
#define DH_VERSION_MINOR 0
