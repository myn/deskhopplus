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
    case listenerDetected
    /*
     * The board granted a pairing under a different identity key from the one
     * this helper had pinned. That is a board wiped past its identity sector,
     * re-flashed, or swapped for another — and the one case where the chord
     * must *not* be offered, because pressing it is precisely how a swapped
     * board would be accepted (#112).
     */
    case boardIdentityChanged

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
        case .listenerDetected:
            return "Listener detected — another process is probing the channel"
        case .boardIdentityChanged:
            return "Device identity changed — if you re-flashed it, remove the pinned board key"
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
     *
     * `listenerDetected` is the same reading again. The session is negotiated
     * and authenticated; what the board detected is somebody *writing* frames
     * it refused, which the tag already keeps out of this session. Refusing
     * bulk here would disagree with `canSendBulk` and give #52 two answers.
     * What a listener can still do is *read* a payload in clear — and the
     * remedy for that is sealing it (#113), not withholding it on a signal
     * that a passive listener never trips.
     */
    public var allowsBulkTransfers: Bool {
        self == .connected || self == .reconnectingRepeatedly || self == .listenerDetected
    }
}
