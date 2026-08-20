import DHCore

/*
 * What the user is told, in words. The *distinctions* are the shared core's
 * (`dh_helper_state`, #80) and so is the policy read off them; what lives here
 * is the wording, because a Windows tray tooltip and a macOS menu bar item are
 * not one string table living in C.
 *
 * The distinction that carries a security property: **only `notPaired` prompts
 * the config chord**, because the chord provisions whatever is attached to the
 * channel during its window (#34). Two states here are precisely the ones a
 * chord press would make worse — `listenerDetected`, where something else is
 * writing to the channel, and `boardIdentityChanged`, where pressing it is the
 * act that accepts a swapped board. That rule is *not* re-decided here: both
 * predicates below call the core's, so a second helper cannot answer them
 * differently.
 *
 * `channelHeld` — *"Another program holds the channel"* — used to carry that
 * rule and is gone (#72, #114, ADR-0008). It asserted something that can never
 * be true on macOS: a second `kIOHIDOptionsTypeSeizeDevice` open succeeds,
 * measured on 2026-08-13, so the open is never refused for the reason the
 * message named. What replaces it is `listenerDetected`, which is measured
 * rather than assumed — the board counts frames it could not authenticate and
 * says so.
 */
public enum HelperState: UInt32, CaseIterable, Equatable {
    /* Looking, or briefly gone. Nothing is shown to the user: a device that
       disappears for a moment is ordinary, and config mode is something the
       user did on purpose. */
    case quiet = 0

    case connected = 1
    /*
     * The connection keeps having to be rebuilt. Each cycle on its own is
     * correctly too brief to report — and a helper failing every frame it
     * received once spent two days saying `Connected and paired` on exactly
     * that reasoning (#94). This is what a rate says that no single cycle can.
     */
    case reconnectingRepeatedly = 2
    case notPaired = 3
    case deviceInConfigMode = 4
    case deviceAbsent = 5
    case versionIncompatible = 6
    /*
     * Something other than this helper is writing frames the board could not
     * authenticate, at a rate the board measured and reported (#111). It says
     * only that: a listener that merely *reads* writes nothing to refuse and
     * cannot be detected at all, here or anywhere else in the protocol, so
     * silence here is not a clean channel.
     */
    case listenerDetected = 7
    /*
     * The board granted a pairing under a different identity key from the one
     * this helper had pinned. That is a board wiped past its identity sector,
     * re-flashed, or swapped for another — and the one case where the chord
     * must *not* be offered, because pressing it is precisely how a swapped
     * board would be accepted (#112).
     */
    case boardIdentityChanged = 8

    /* The raw values above are `dh_helper_state`'s, so the two conversions are
       arithmetic rather than a switch a new state could be left out of. The
       pairing is asserted state by state in the tests. */
    init?(core: dh_helper_state) { self.init(rawValue: core.rawValue) }
    var core: dh_helper_state { dh_helper_state(rawValue: rawValue) }

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

    /* The chord remedy is shown only from `notPaired`. Decided by the core, so
       that the #34 property has one answer across both helpers. */
    public var promptsConfigChord: Bool { dh_helper_prompts_config_chord(core) }

    /*
     * An incompatible peer keeps placement and refuses bulk: a misparsed
     * placement puts the cursor somewhere wrong and self-corrects, while a
     * misparsed chunk header writes a corrupted file presented as valid.
     *
     * Also the core's, and it must keep agreeing with
     * `HelperSession.canSendBulk` — the seam #52 consumes. This answers for
     * what the user is being told; that one answers for the session.
     */
    public var allowsBulkTransfers: Bool { dh_helper_allows_bulk(core) }
}
