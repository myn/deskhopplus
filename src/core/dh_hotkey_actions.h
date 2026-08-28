#pragma once

/* Single ordered catalog shared by firmware, protocol, and webconfig/render.py. */
#define DH_HOTKEY_CONFIG_FIELD_BASE 110
#define DH_HOTKEY_ACTIONS(X) \
    X(OUTPUT_TOGGLE, output_toggle) \
    X(MOUSE_ZOOM, mouse_zoom) \
    X(SWITCHLOCK, switch_lock) \
    X(SCREENLOCK, screen_lock) \
    X(GAMING_MODE, gaming_mode) \
    X(SCREENSAVER_PONG, screensaver_pong) \
    X(SCREENSAVER_JITTER, screensaver_jitter) \
    X(SCREENSAVER_DISABLE, screensaver_disable) \
    X(WIPE_CONFIG, wipe_config) \
    X(SCREEN_SEAM, screen_seam) \
    X(CONFIG_ENABLE, config_mode) \
    X(FW_UPGRADE_A, firmware_upgrade_a) \
    X(FW_UPGRADE_B, firmware_upgrade_b)
