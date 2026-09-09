import Foundation

/// Registration for the next GUI login. Never unload the running helper: doing
/// so would interrupt a transfer just because the user changed a preference.
struct LaunchAtLogin {
    let plist: URL
    let executable: String

    init(plist: URL = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/LaunchAgents/com.deskhopplus.helper.plist"),
         executable: String = Bundle.main.executableURL!.path) {
        self.plist = plist
        self.executable = executable
    }

    private var saved: URL { plist.appendingPathExtension("disabled") }
    var isEnabled: Bool { FileManager.default.fileExists(atPath: plist.path) }

    func setEnabled(_ enabled: Bool) throws {
        guard enabled != isEnabled else { return }
        let files = FileManager.default
        if !enabled {
            // Do not overwrite a saved job: a conflict needs a human decision.
            try files.moveItem(at: plist, to: saved)
        } else if files.fileExists(atPath: saved.path) {
            try files.moveItem(at: saved, to: plist)
        } else {
            try files.createDirectory(at: plist.deletingLastPathComponent(),
                                      withIntermediateDirectories: true)
            let job: [String: Any] = [
                "Label": "com.deskhopplus.helper",
                "ProgramArguments": [executable],
                "RunAtLoad": true,
                "KeepAlive": ["SuccessfulExit": false],
                "ThrottleInterval": 10,
                "ProcessType": "Interactive",
                "StandardErrorPath": "/tmp/deskhop-helper.log",
            ]
            let data = try PropertyListSerialization.data(fromPropertyList: job,
                                                          format: .xml, options: 0)
            try data.write(to: plist, options: .atomic)
        }
    }
}
