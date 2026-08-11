import DHCore

/*
 * How the helper finds the device — by USB identifier, serial, usage page and
 * usage, never by a device path. Paths are not stable across reconnects, and
 * the interface disappearing and returning is normal operation here: entering
 * config mode reboots the device under a *different* USB identity for up to
 * five minutes, then reboots back (ADR-0001).
 */
public enum ChannelIdentity {
    /* Normal mode: pid.codes/1209/C000. The channel exists only here. */
    public static let vendorID = 0x1209
    public static let productID = 0xC000

    /* Config mode. Seeing this is not the device being absent — it is the
       user opening the configuration page, and the helper says so. */
    public static let configVendorID = 0x2E8A
    public static let configProductID = 0x107C

    /*
     * Match on the vendor page and the channel's own usage, nothing wider.
     * Broad matching would open a keyboard and trigger an Input Monitoring
     * prompt — the mistake behind most claims that HID needs one.
     */
    public static let usagePage = 0xFF00
    public static let usage = 0x20

    /* One report is one full-speed packet, and the framing layer owns every
       byte of it: no report ID, so no byte is spent on one. */
    public static let reportSize = 64

    /* What this helper asks for in its hello. The device answers with the
       effective values, which are what the session actually runs with. */
    public static let requestedChannelCount = UInt8(DH_SESSION_CHANNEL_COUNT)
    public static let requestedMaxChunk = UInt16(DH_SESSION_MAX_CHUNK)
}
