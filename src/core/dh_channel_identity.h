#pragma once

/* USB identity shared by firmware and helpers. The channel exists only in
   normal mode; config mode reboots under a different device identity. */
#define DH_CHANNEL_VENDOR_ID 0x1209
#define DH_CHANNEL_PRODUCT_ID 0xC000
#define DH_CHANNEL_CONFIG_VENDOR_ID 0x2E8A
#define DH_CHANNEL_CONFIG_PRODUCT_ID 0x107C
#define DH_CHANNEL_USAGE_PAGE 0xFF00
/* Channel n uses this base usage + n. */
#define DH_CHANNEL_USAGE 0x20
/* No report ID: one report is one full-speed interrupt packet. */
#define DH_CHANNEL_REPORT_SIZE 64
