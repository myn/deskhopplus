/*
 * What the user is told, named in words with its remedy (#38).
 *
 * The distinction that carries a security property: `channelHeld` and
 * `notPaired` are different states with different remedies. A refused open
 * must never prompt the config chord, because the program holding the channel
 * is exactly what the chord would provision (#34) — so only `notPaired`
 * prompts it, and it is reachable only when this helper holds every channel
 * itself and the device refused it on authentication.
 */
public enum HelperState: Equatable {
    /* Looking, or briefly gone. Nothing is shown to the user: a device that
       disappears for a moment is ordinary, and config mode is something the
       user did on purpose. */
    case quiet

    case connected
    /*
     * The connection keeps having to be rebuilt. Each cycle on its own is
     * correctly too brief to report — and a helper failing every frame it
     * received once spent two days saying `Connected and paired` on exactly
     * that reasoning (#94). This is what a rate says that no single cycle can.
     */
    case reconnectingRepeatedly
    case notPaired
    case channelHeld
    case deviceInConfigMode
    case deviceAbsent
    case versionIncompatible

    public var message: String? {
        switch self {
        case .quiet: return nil
        case .connected: return "Connected and paired"
        case .reconnectingRepeatedly:
            return "Reconnecting repeatedly — check the link, and that the helper is up to date"
        case .notPaired: return "Not paired — press the config chord on the device"
        case .channelHeld: return "Another program holds the channel — find and stop it"
        case .deviceInConfigMode: return "Device in config mode"
        case .deviceAbsent: return "Device not connected"
        case .versionIncompatible:
            return "Helper version does not match the device — file transfers are refused"
        }
    }

    /* The chord remedy is shown only here. See the type's note. */
    public var promptsConfigChord: Bool { self == .notPaired }

    /*
     * An incompatible peer keeps placement and refuses bulk: a misparsed
     * placement puts the cursor somewhere wrong and self-corrects, while a
     * misparsed chunk header writes a corrupted file presented as valid.
     *
     * A connection that keeps being rebuilt is *not* one of those: while it
     * is up it is a negotiated session like any other, and this must keep
     * agreeing with `SessionEngine.canSendBulk`, the seam #52 consumes.
     * Reporting how often it is rebuilt changes what the user is told, not
     * what the session may carry.
     */
    public var allowsBulkTransfers: Bool { self == .connected || self == .reconnectingRepeatedly }
}
