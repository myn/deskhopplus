/*
 * What the user is told, named in words with its remedy (#38).
 *
 * The distinction that carries a security property: **only `notPaired` prompts
 * the config chord**, because the chord provisions whatever is attached to the
 * channel during its window (#34). Two states here are precisely the ones a
 * chord press would make worse — `listenerDetected`, where something else is
 * writing to the channel, and `boardIdentityChanged`, where pressing it is the
 * act that accepts a swapped board — so the rule is asserted by a test rather
 * than left to a reading of this comment.
 *
 * `channelHeld` — *"Another program holds the channel"* — used to carry that
 * rule and is gone (#72, #114, ADR-0008). It asserted something that can never
 * be true on macOS: a second `kIOHIDOptionsTypeSeizeDevice` open succeeds,
 * measured on 2026-08-13, so the open is never refused for the reason the
 * message named. What replaces it is `listenerDetected`, which is measured
 * rather than assumed — the board counts frames it could not authenticate and
 * says so.
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
    case deviceInConfigMode
    case deviceAbsent
    case versionIncompatible
    /*
     * Something other than this helper is writing frames the board could not
     * authenticate, at a rate the board measured and reported (#111). It says
     * only that: a listener that merely *reads* writes nothing to refuse and
     * cannot be detected at all, here or anywhere else in the protocol, so
     * silence here is not a clean channel.
     */
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
        case .deviceInConfigMode: return "Device in config mode"
        case .deviceAbsent: return "Device not connected"
        case .versionIncompatible:
            return "Helper version does not match the device — file transfers are refused"
        case .listenerDetected:
            return "Another program is writing to the device channel — find and stop it, "
                 + "and do not press the config chord while it is running"
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
