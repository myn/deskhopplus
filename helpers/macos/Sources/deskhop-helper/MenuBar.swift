import AppKit
import CoreGraphics
import DeskhopChannel
import Foundation

/*
 * The macOS helper's presence in the menu bar (#54's first slice, built here
 * because #56 needs it).
 *
 * Until now this helper had no user interface at all: it ran from a LaunchAgent
 * and wrote to a log nobody has open. That was survivable while everything it
 * carried was instant. It stops being survivable the moment a paste can take
 * four minutes and has to be *agreed to* first — the acceptance, the
 * progress and the abort all need somewhere to live, and #42 decided that
 * somewhere is a permanent menu-bar presence rather than a window that appears
 * and goes.
 *
 * ---------------------------------------------------------------------------
 * WHY THE QUESTION IS A PANEL AND NOT AN ALERT
 *
 * `NSAlert.runModal` blocks the run loop, and this helper's run loop is what
 * carries the session's heartbeat — so an unanswered dialog would time the
 * session out and drop the very transfer it is asking about. The panel below
 * is non-activating and non-modal: it takes no focus from what the user is
 * typing into, and the loop keeps turning underneath it.
 *
 * The menu carries the same two answers, so a panel dismissed or missed does
 * not strand the offer.
 *
 * ---------------------------------------------------------------------------
 * THIS NEEDS A WINDOW SERVER
 *
 * `NSStatusItem` does. The LaunchAgent runs in the user's GUI session
 * (`gui/$UID`, ProcessType Interactive), so it has one. Running the helper
 * over ssh with nobody logged in does not, and is not a supported arrangement
 * — the helper reads this machine's pasteboard, which needs the same session.
 */
final class MenuBar: NSObject, NSMenuDelegate {
    struct Callbacks {
        let acceptFiles: (UInt32) -> Void
        let declineFiles: (UInt32) -> Void
        let abortTransfer: () -> Void
        /* Whether something is on its way *out* of this computer, and how to
           stop it (#42, story 7 — a mis-copied folder must not hold anyone
           hostage). Asked rather than pushed: the menu is filled when it opens,
           so it can simply look. */
        let isSending: () -> Bool
        let abortSend: () -> Void
        let quit: () -> Void
    }

    /// How often the progress line is refreshed while something is arriving.
    /// Half a second: fast enough to look alive, slow enough that a transfer
    /// is not paying for its own progress display.
    static let progressInterval: TimeInterval = 0.5

    /// Diagnostics, never shown to the user.
    var log: ((String) -> Void)?

    private var item: NSStatusItem?
    private var callbacks: Callbacks?
    private var state: HelperState = .quiet
    private var question: FileOffer?
    private var panel: NSPanel?
    private var progress: (received: UInt64, total: UInt64)?

    /*
     * Whether there is a window server to attach to.
     *
     * `NSStatusBar.system` does not fail politely without one: CoreGraphics
     * asserts and aborts the process, which no `catch` in this language can
     * see. So the question is asked before the window server is touched at
     * all, and a login session that has none — an ssh login, a launchd job
     * outside the GUI domain — simply gets no menu bar.
     *
     * The session dictionary is the documented way to ask. It is nil outside a
     * GUI login session and is safe to call with no connection, which is the
     * whole reason it is what is asked.
     */
    static var canAttach: Bool { CGSessionCopyCurrentDictionary() != nil }

    func attach(callbacks: Callbacks) {
        /*
         * `NSApp` is nil until something has created the application, and
         * `NSStatusBar.system` reached before that aborts the process inside
         * CoreGraphics rather than failing. Checked here, at the one place
         * that touches the status bar, so that getting the caller's ordering
         * wrong costs a log line and no menu bar — never the helper.
         *
         * The rule this enforces is #42's, one layer in: cursor placement and
         * the clipboard are what this process is for, and neither may be lost
         * because a status item could not be made.
         */
        guard NSApp != nil else {
            log?("the menu bar was asked for before the application existed, so there is none; "
                 + "cursor placement and the clipboard are unaffected")
            return
        }
        self.callbacks = callbacks
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.button?.title = Self.idleTitle
        /* One menu for the life of the item; its contents are filled in when it
           is about to open (`menuNeedsUpdate`), so nothing else here has to
           remember to rebuild it. */
        let menu = NSMenu()
        menu.delegate = self
        item.menu = menu
        self.item = item
        updateTitle()
    }

    /// The device state, in the words the shared core's state is given
    /// (`HelperState.message`).
    func show(state: HelperState) {
        guard self.state != state else { return }
        self.state = state
        updateTitle()
    }

    /// Files are being offered from the other computer and nothing has crossed
    /// the link yet.
    func ask(about offer: FileOffer) {
        question = offer
        updateTitle()
        showPanel(for: offer)
    }

    /// The question no longer stands, however it was answered.
    func withdrawQuestion(id: UInt32) {
        guard question?.id == id else { return }
        question = nil
        closePanel()
        updateTitle()
    }

    /// How far the arriving transfer has got, or nil when nothing is arriving.
    func show(progress: (received: UInt64, total: UInt64)?) {
        self.progress = progress
        updateTitle()
    }

    /*
     * The menu is built when it is about to be shown, and never while it is
     * open.
     *
     * Rebuilding on every change would be the obvious thing and is wrong twice
     * over: progress moves twice a second, and replacing an `NSMenu` the user
     * has open closes it under them — including at the moment they are
     * reaching for Accept. Building here instead means what they see is always
     * current, and nothing is thrown away underneath them.
     */
    func menuNeedsUpdate(_ menu: NSMenu) { fill(menu) }

    // MARK: - The menu

    private static let idleTitle = "⌥"

    private func updateTitle() {
        guard let button = item?.button else { return }
        if question != nil {
            button.title = "⬇ files?"
        } else if let progress, progress.total > 0 {
            button.title = "⬇ \(Self.percent(progress))%"
        } else {
            button.title = Self.idleTitle
        }
        button.toolTip = state.message ?? "deskhopplus helper"
    }

    private func fill(_ menu: NSMenu) {
        menu.removeAllItems()
        menu.autoenablesItems = false

        let status = NSMenuItem(title: state.message ?? "Waiting for the device",
                                action: nil, keyEquivalent: "")
        status.isEnabled = false
        menu.addItem(status)

        if let question {
            menu.addItem(.separator())
            let summary = NSMenuItem(title: Self.summary(of: question), action: nil,
                                     keyEquivalent: "")
            summary.isEnabled = false
            menu.addItem(summary)
            menu.addItem(action("Accept and start the transfer", #selector(accept)))
            menu.addItem(action("Decline", #selector(decline)))
        }

        if let progress, progress.total > 0 {
            menu.addItem(.separator())
            let line = NSMenuItem(
                title: "Receiving \(Self.size(progress.received)) of "
                    + "\(Self.size(progress.total)) — \(Self.percent(progress))%",
                action: nil, keyEquivalent: "")
            line.isEnabled = false
            menu.addItem(line)
            menu.addItem(action("Cancel this transfer", #selector(abort)))
        }

        if callbacks?.isSending() == true {
            menu.addItem(.separator())
            menu.addItem(action("Cancel what is being sent", #selector(abortSend)))
        }

        menu.addItem(.separator())
        menu.addItem(action("Quit deskhopplus helper", #selector(quit)))
    }

    private func action(_ title: String, _ selector: Selector) -> NSMenuItem {
        let entry = NSMenuItem(title: title, action: selector, keyEquivalent: "")
        entry.target = self
        entry.isEnabled = true
        return entry
    }

    @objc private func accept() {
        guard let question else { return }
        closePanel()
        self.question = nil
        updateTitle()
        callbacks?.acceptFiles(question.id)
    }

    @objc private func decline() {
        guard let question else { return }
        closePanel()
        self.question = nil
        updateTitle()
        callbacks?.declineFiles(question.id)
    }

    @objc private func abort() { callbacks?.abortTransfer() }
    @objc private func abortSend() { callbacks?.abortSend() }
    @objc private func quit() { callbacks?.quit() }

    // MARK: - The panel that asks

    private func showPanel(for offer: FileOffer) {
        closePanel()

        let width: CGFloat = 360
        let height: CGFloat = 118
        let panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: width, height: height),
                            styleMask: [.titled, .nonactivatingPanel, .utilityWindow],
                            backing: .buffered, defer: false)
        panel.title = "Files from the other computer"
        panel.level = .floating
        panel.hidesOnDeactivate = false
        /* Non-activating, and it must stay that way: this helper's user is
           typing into something else, and stealing their focus to ask a
           question is worse than the question going unanswered. */
        panel.becomesKeyOnlyIfNeeded = true

        let text = NSTextField(labelWithString: Self.summary(of: offer))
        text.frame = NSRect(x: 16, y: 58, width: width - 32, height: 44)
        text.lineBreakMode = .byWordWrapping
        text.maximumNumberOfLines = 3
        panel.contentView?.addSubview(text)

        let decline = NSButton(title: "Decline", target: self, action: #selector(decline))
        decline.frame = NSRect(x: width - 200, y: 16, width: 88, height: 30)
        decline.bezelStyle = .rounded
        panel.contentView?.addSubview(decline)

        let accept = NSButton(title: "Accept", target: self, action: #selector(accept))
        accept.frame = NSRect(x: width - 104, y: 16, width: 88, height: 30)
        accept.bezelStyle = .rounded
        accept.keyEquivalent = "\r"
        panel.contentView?.addSubview(accept)

        if let screen = NSScreen.main {
            let frame = screen.visibleFrame
            panel.setFrameTopLeftPoint(NSPoint(x: frame.maxX - width - 20, y: frame.maxY - 20))
        }
        panel.orderFrontRegardless()
        self.panel = panel
    }

    private func closePanel() {
        panel?.orderOut(nil)
        panel = nil
    }

    // MARK: - Words

    /// What the user is being asked to agree to: how many files, how big, and
    /// how long it will take. The duration is the point — a size alone does
    /// not tell anyone whether to wait (#39, #56).
    static func summary(of offer: FileOffer) -> String {
        let count = offer.files.count == 1
            ? offer.files[0].name
            : "\(offer.files.count) files"
        return "\(count) — \(size(offer.total)), about \(duration(offer.estimatedSeconds))."
    }

    /// Integer arithmetic, and truncating rather than rounding — the same
    /// spelling as `Tray::size_text` on the other computer, so the two ends
    /// quote one transfer at one size. A float here would also put a decimal
    /// comma in front of some users.
    static func size(_ bytes: UInt64) -> String {
        if bytes >= 1024 * 1024 {
            let tenths = (bytes * 10) / (1024 * 1024)
            return "\(tenths / 10).\(tenths % 10) MB"
        }
        if bytes >= 1024 { return "\(bytes / 1024) KB" }
        return "\(bytes) bytes"
    }

    static func duration(_ seconds: Int) -> String {
        if seconds < 60 { return "\(max(seconds, 1)) seconds" }
        let minutes = (seconds + 59) / 60
        return minutes == 1 ? "a minute" : "\(minutes) minutes"
    }

    static func percent(_ progress: (received: UInt64, total: UInt64)) -> Int {
        guard progress.total > 0 else { return 0 }
        return Int(progress.received * 100 / progress.total)
    }
}
