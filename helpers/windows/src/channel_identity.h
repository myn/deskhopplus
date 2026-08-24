#pragma once
/*
 * How the helper finds the device — by USB identifier, serial and usage page,
 * never by a device interface path. Paths are not stable across reconnects on
 * either platform, and the interface disappearing and returning is normal
 * operation here: entering config mode reboots the device under a *different*
 * USB identity for up to five minutes, then reboots back (ADR-0001).
 *
 * The macOS twin is ChannelIdentity.swift. These are the same numbers said in
 * another language, and they are the only numbers this helper owns — every
 * negotiated value comes off the device's reply (dh_helper.negotiated).
 */

#include <cstddef>
#include <cstdint>


namespace deskhop {

/* Normal mode: pid.codes/1209/C000. The channel exists only here. */
inline constexpr uint16_t kVendorId = 0x1209;
inline constexpr uint16_t kProductId = 0xC000;

/* Config mode. Seeing this is not the device being absent — it is the user
   opening the configuration page, and the helper says so. */
inline constexpr uint16_t kConfigVendorId = 0x2E8A;
inline constexpr uint16_t kConfigProductId = 0x107C;

/*
 * Match on the vendor page and the channel's own usage, nothing wider. Broad
 * matching would open a keyboard, which on Windows is a device the user's
 * security software watches and on macOS raises an Input Monitoring prompt.
 */
inline constexpr uint16_t kUsagePage = 0xFF00;
inline constexpr uint16_t kUsage = 0x20;

/* One report is one full-speed packet, and the framing layer owns every byte
   of it: no report ID, so no byte is spent on one.

   Windows still prepends a report-ID byte to every buffer it hands to and
   takes from a HID collection, even one that declares no report IDs — so the
   transport's buffers are one byte longer than this and hid_transport.cpp
   reads their real length off HIDP_CAPS rather than adding one here. */
inline constexpr size_t kReportSize = 64;

/*
 * What the hello asks for is *not* here. The core builds the hello itself, off
 * DH_SESSION_CHANNEL_COUNT and DH_SESSION_MAX_CHUNK, and the device answers
 * with the effective values — which arrive back as dh_helper.negotiated and
 * are what the session actually runs with (ADR-0002). Restating either number
 * on this side would give one request two sources.
 */

} // namespace deskhop
