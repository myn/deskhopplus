import AppKit
import DeskhopChannel
import DHCore
import Foundation
import Security

/*
 * The loop: transport events into the session, its outputs back out to the
 * transport. Nothing here is decided here — every decision is the shared C
 * core's, reached through HelperSession. This file carries messages and owns
 * the clock.
 *
 * What each output *means* is OutputDispatch, in DeskhopChannel, so that every
 * arm of it is reachable by a test (#152). This file is the IOKit-and-AppKit
 * half of that seam: it implements `HelperEffects` over the real transport,
 * pasteboard and Keychain, in a line or two each.
 */
final class HelperRuntime: HelperEffects {
    private let secrets = SecretStore()
    private let session: HelperSession
    private let transport = ChannelTransport()
    private let clipboard: ClipboardService
    private let pasteboard = Pasteboard()
    private let cursorPlacement = CursorPlacement()
    /* Where files that arrive are written, and the menu bar that asks about
       them, shows how far they have got, and lets the user stop them (#56). */
    private let files = FileStore()
    private let menuBar = MenuBar()

    /*
     * Whether the last thing the session said was that bulk may cross. The
     * clipboard has to be told when a session *ends* — its seal and any
     * transfer go with it — and the session reports a state rather than an
     * event, so the transition is worked out here.
     */
    private var bulkWasAllowed = false
    private var imagePrefetch = ImagePrefetch()

    /* What the menu bar last showed, so the twice-a-second refresh only
       touches it when something has actually moved. */
    private var shownProgress: (received: UInt64, total: UInt64)?

    /* Lazy so that `self` is fully formed before the dispatch is handed a
       reference to it. The dispatch holds it unowned; this is the strong half. */
    private lazy var dispatch = OutputDispatch(effects: self)

    init() {
        /*
         * A blob that will not decode is regenerated inside `loadIdentity`, so
         * reaching this is the machine having no Secure Enclave to generate a
         * key in at all — an Intel Mac without a T2. That is a hard
         * requirement of ADR-0008 rather than a fault to recover from, so it
         * names itself and stops instead of retrying something that cannot
         * work. launchd will restart it; the log line says why each time.
         */
        let identity: EnclaveIdentity
        do {
            identity = try secrets.loadIdentity()
        } catch {
            Self.note("no Secure Enclave identity, and one could not be created: \(error). "
                      + "This helper requires a Mac with a Secure Enclave.")
            exit(EXIT_FAILURE)
        }
        /*
         * The key id every hello carries, and the value the board's config page
         * shows as *Paired helper* (#114) — same byte order, same spelling, so
         * the two can be compared by eye. Without it the page answers "what is
         * paired to this board?" with a number nothing else in the system ever
         * prints.
         */
        Self.note("helper key id: " + identity.keyId.map { String(format: "%02x", $0) }.joined())

        let boardKey = secrets.loadBoardKey()

        session = HelperSession(
            identity: identity, boardPublicKey: boardKey,
            entropy: Self.entropy)
        /* The seal draws ephemeral keys and nonces from the same source the
           session's correlation values come from. A short draw would key a
           seal on bytes nobody chose, which `Self.entropy` refuses to do. */
        clipboard = ClipboardService(entropy: Self.entropy)
    }

    private static func entropy(_ count: Int) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: count)
        guard SecRandomCopyBytes(kSecRandomDefault, count, &bytes) == errSecSuccess else {
            fatalError("SecRandomCopyBytes failed")
        }
        return bytes
    }

    private static let started = ProcessInfo.processInfo.systemUptime
    private static let stamp = LogStamp()

    /// The session's tick — fine enough that a heartbeat is never late by much.
    static let tickInterval: TimeInterval = 0.25

    /*
     * Monotonic, deliberately. `Date()` is not: a backwards clock correction
     * of more than a couple of seconds — routine on a laptop coming out of
     * sleep — would stall the heartbeat past the device's three-second
     * timeout and kill a healthy session. The log prints this beside the wall
     * clock rather than instead of it (#103); LogStamp says why both.
     */
    private static var elapsed: TimeInterval {
        /* The origin is read *first*, deliberately. `started` is a lazy static,
           so the very first reading initialises it — and had this been written
           as one subtraction, Swift would evaluate the left operand before
           triggering that initialisation, making the first log line of every
           run a negative elapsed. It did, and the first real run showed it. */
        let origin = started
        return ProcessInfo.processInfo.systemUptime - origin
    }

    /// The session's clock, and the same origin the log's elapsed column counts from.
    private var now: TimeInterval { Self.elapsed }

    func run() {
        transport.log = { message in Self.note(message) }
        transport.onEvent = { [weak self] event in self?.feed(event) }

        cursorPlacement.log = { message in Self.note(message) }

        /* Verified payloads, straight from the core. Nothing here re-reads
           the stream: decode, tag and replay counter are all upstream of this. */
        session.onPayload = { [weak self] type, body in
            guard let self else { return }
            if self.cursorPlacement.received(type: type, body: body) { return }
            if type == UInt8(DH_MSG_POS_QUERY.rawValue),
               body.count == Int(DH_POS_QUERY_BODY_SIZE),
               let response = self.cursorPlacement.positionBody(queryID: body[0]) {
                Self.note("cursor query id=\(body[0]) received")
                if self.sendPayload(type: UInt8(DH_MSG_POS_RESPONSE.rawValue), body: response) {
                    Self.note("cursor response id=\(body[0]) sent")
                }
                return
            }
            self.dispatch.emit(self.clipboard.received(type: type, body: body))
        }

        pasteboard.log = { message in Self.note(message) }
        pasteboard.onLocalCopy = { [weak self] text in
            guard let self else { return }
            /* Nothing is offered without a session to carry it. The state the
               user is shown and this answer come from the same core, so a
               helper that says "connected" and refuses a copy is not a state
               this can reach. */
            guard self.session.canSendBulk else { return }
            self.dispatch.emit(self.clipboard.localCopy(kind: .text, bytes: Array(text.utf8)))
        }
        pasteboard.onLocalImage = { [weak self] bytes in
            guard let self, self.session.canSendBulk else { return }
            self.dispatch.emit(self.clipboard.localCopy(kind: .png, bytes: bytes))
        }
        pasteboard.onLocalFiles = { [weak self] copied in
            guard let self, self.session.canSendBulk else { return }
            /* The list goes out now; `read` is not called until the other
               computer's user accepts the transfer. That is the whole of #56's
               "transfer begins on paste, not on copy". */
            self.dispatch.emit(self.clipboard.localCopy(files: copied.entries,
                                                        provider: copied.read))
        }
        pasteboard.onLocalReplacement = { [weak self] in
            guard let self, let id = self.imagePrefetch.localReplacement() else { return }
            self.dispatch.emit(self.clipboard.lazyImageWasReplaced(id: id))
        }

        files.log = { message in Self.note(message) }
        /* Emptied at start, and only at start: a helper that crashes never
           runs an exit path, and a timer would delete a file the user is still
           working on (FileStore). */
        files.collectGarbage()

        menuBar.attach(callbacks: MenuBar.Callbacks(
            acceptFiles: { [weak self] id in
                guard let self else { return }
                self.dispatch.emit(self.clipboard.acceptFiles(id: id))
            },
            declineFiles: { [weak self] id in
                guard let self else { return }
                self.dispatch.emit(self.clipboard.declineFiles(id: id))
            },
            abortTransfer: { [weak self] in
                guard let self else { return }
                self.dispatch.emit(self.clipboard.abortReceive())
            },
            isSending: { [weak self] in self?.clipboard.awaitingSend ?? false },
            abortSend: { [weak self] in
                guard let self else { return }
                self.dispatch.emit(self.clipboard.abortSend())
            },
            quit: { NSApplication.shared.terminate(nil) }))

        transport.start()
        pasteboard.start()

        Timer.scheduledTimer(withTimeInterval: Self.tickInterval, repeats: true) { [weak self] _ in
            self?.feed(.tick)
        }
        /* Separate from the session tick, and slower: what the menu bar shows
           changes at human speed, and rebuilding a menu the user has open
           closes it under them. */
        Timer.scheduledTimer(withTimeInterval: MenuBar.progressInterval,
                             repeats: true) { [weak self] _ in
            self?.refreshProgress()
        }

        Self.note("deskhop helper started; waiting for the channel")
        /*
         * `NSApplication.run`, not `RunLoop.current.run`: the menu bar item is
         * AppKit and needs an application to belong to. Every timer above is
         * on the main run loop, which this turns.
         *
         * `.accessory` is what keeps that from costing a Dock icon and an app
         * switcher entry — this is a background helper, and it stays one.
         */
        NSApplication.shared.setActivationPolicy(.accessory)
        NSApplication.shared.run()
    }

    /// Push the arriving transfer's progress to the menu bar, and only when it
    /// has moved.
    private func refreshProgress() {
        let arriving = clipboard.arriving
        let now = arriving.map { (received: $0.received, total: $0.total) }
        let changed = now?.received != shownProgress?.received
            || now?.total != shownProgress?.total
        guard changed else { return }
        shownProgress = now
        menuBar.show(progress: now)
    }

    private func feed(_ input: SessionInput) {
        dispatch.apply(session.handle(input, at: now))

        /*
         * A session that has gone takes the seal and any transfer with it.
         *
         * Asked of `canSendBulk` — the *session's* answer — and on every input,
         * not of the state the user is shown and not only when that state
         * changes. `dh_helper_allows_bulk` counts `reconnectingRepeatedly` as
         * allowing bulk, and that is precisely the state a teardown lands in
         * once the flap rate has tripped: the session is gone, its keys are
         * cleared, and the state reads `true` before and after. Worse, the core
         * reports that state only on the transition, so the second and
         * subsequent drops of a burst produce no state output at all. #107
         * measured 586 teardowns in sixteen hours — the exact condition that
         * trips the rate — so the edge that matters is the one this misses.
         */
        let live = session.canSendBulk
        if bulkWasAllowed && !live {
            dispatch.emit(clipboard.sessionEnded())
        }
        bulkWasAllowed = live

        /* A chance to push the next credit-gated batch. On the tick as well as
           on arriving frames, so a transfer whose last credit grant was lost
           still finishes rather than sitting still. */
        if live {
            dispatch.emit(clipboard.pump())
        }
        /* And a chance to give up on one that has stopped moving — the far
           helper having crashed leaves this end's session perfectly healthy,
           so nothing else here would ever notice. */
        if case .tick = input {
            /* The board's drop totals go with the tick so that an abandonment
               can quote them (#133). Read here rather than held there: the
               board restates them whenever they move, and nothing tells the
               clipboard when that was. */
            dispatch.emit(clipboard.tick(at: now, boardDrops: session.boardDrops))
        }
    }

    /* A cursor-position response, which is not an output of either service —
       the placement machine answers a query directly — so it does not go
       through the dispatch. The rule it follows is the same one: the idle
       timer is charged only for a frame the transport actually took. */
    private func sendPayload(type: UInt8, body: [UInt8]) -> Bool {
        guard let frame = session.emit(type: type, body: body) else {
            Self.note("a cursor-position response could not be built; there is no session")
            return false
        }
        if transport.send(frame) {
            session.noteSent(at: now)
            return true
        } else {
            session.noteSendRefused()
            Self.note("a cursor-position response was not taken by the transport and is lost")
            return false
        }
    }

    /* ------------------------------------------------------------ HelperEffects
       The platform half of OutputDispatch's seam: one line each over the real
       object. What each output *means* is OutputDispatch's (#152). */

    func storeBoardKey(_ key: [UInt8]) -> Bool { secrets.saveBoardKey(key) }
    func acquireChannels() { transport.acquire() }
    func releaseChannels() { transport.release() }
    func send(_ frame: [UInt8]) -> Bool { transport.send(frame) }
    func buildFrame(type: UInt8, body: [UInt8]) -> [UInt8]? {
        session.emit(type: type, body: body)
    }
    func noteSent() { session.noteSent(at: now) }
    func noteSendRefused() { session.noteSendRefused() }
    func show(state: HelperState) { menuBar.show(state: state) }
    func deliver(text bytes: [UInt8]) { pasteboard.deliver(text: bytes) }
    func deliver(image bytes: [UInt8]) {
        switch imagePrefetch.complete() {
        case .ordinary:
            pasteboard.deliver(image: bytes)
        case .publish(let changeCount):
            pasteboard.deliver(image: bytes, ifUnchangedSince: changeCount)
        }
    }
    func lazyImage(id: UInt32, total: UInt64) {
        let requestID = imagePrefetch.begin(id: id, changeCount: pasteboard.changeCount)
        Self.note("prefetching remote image \(id) of \(total) bytes without claiming the Mac "
                  + "pasteboard")
        dispatch.emit(clipboard.requestLazyImage(id: requestID))
    }
    func cancelLazyImage(id: UInt32) { imagePrefetch.cancel(id: id) }

    func askAboutFiles(_ offer: FileOffer) {
        Self.note("\(offer.files.count) file(s), \(offer.total) bytes, offered from the other "
                  + "computer; waiting for an answer here before anything crosses")
        menuBar.ask(about: offer)
    }

    func withdrawFileQuestion(id: UInt32) { menuBar.withdrawQuestion(id: id) }

    /* Written first, then referenced. The order is the guarantee: a reference
       only ever points at a set that is complete on disk, so a failed write
       leaves the pasteboard alone rather than pointing at half a file. */
    func deliver(files delivery: FileDelivery) {
        guard let written = self.files.write(delivery) else {
            Self.note("\(delivery.files.count) file(s) arrived and could not be written; "
                      + "nothing was put on the pasteboard")
            return
        }
        if pasteboard.deliver(files: written.urls) {
            Self.note("\(written.urls.count) file(s) written to \(written.directory.path) and "
                      + "put on the pasteboard")
        }
    }

    func clipPolicyChanged(flags: UInt8, capMegabytes: UInt8) -> [ClipboardOutput] {
        clipboard.policyChanged(flags: flags) + clipboard.capacityChanged(megabytes: capMegabytes)
    }
    /* Deliberately the same as `Self.note` below, which every other call site
       in this file uses. An unqualified `note(...)` inside the class reaches
       this one instead, and lands in the same place. */
    func note(_ message: String) { Self.note(message) }

    /* The one effect with a condition of its own: the device may have come
       back by itself while the backoff was running, and re-acquiring channels
       this end already holds is not a no-op. */
    func scheduleRetry(after: TimeInterval) {
        DispatchQueue.main.asyncAfter(deadline: .now() + after) { [weak self] in
            guard let self, self.transport.hasDevice, !self.transport.isHoldingChannels else {
                return
            }
            self.transport.acquire()
        }
    }

    private static func note(_ message: String) {
        let line = stamp.line(message, wall: Date(), elapsed: elapsed)
        FileHandle.standardError.write(Data((line + "\n").utf8))
    }
}
