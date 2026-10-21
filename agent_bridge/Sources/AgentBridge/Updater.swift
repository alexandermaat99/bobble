// Updates: each new build goes up as a GitHub release of the Bobble repo (./build_app.sh --release: tag build-<n>, with
// Bobble.zip). A few times a day the app looks at the newest release; when its build number is higher than ours, it
// downloads the zip, checks it's Bobble, swaps it in for itself and starts again. Files the app downloads itself
// aren't quarantined, so macOS doesn't ask again (only the first, hand-installed copy needs Open Anyway).
// The same release carries the display's firmware (bobble-firmware.bin, built with that build number): once the app
// is current, the board we're paired with gets it too, over Wi-Fi with our key, when no chat is busy.
import AppKit

final class Updater {
    enum Status: Equatable {
        case idle, checking, upToDate, installing(Int), failed(String)
        case boardWaiting(Int), updatingBoard(Int), boardUpdated(Int)   // the display's firmware: till chats finish, going on, done
    }
    private(set) var status: Status = .idle { didSet { if status != oldValue { onChange() } } }
    var onChange: () -> Void = {}

    // Set by build_app.sh in Info.plist: where releases go, and which build this is (yymmddHHMM, so newer is bigger)
    static let repo = Bundle.main.object(forInfoDictionaryKey: "BobbleUpdateRepo") as? String ?? ""
    static let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
    static let build = Int(Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "") ?? 0
    private static let bundleID = "app.bobble.Bobble"

    // Set by the app: the board (firmware goes only to the one we're paired with), and whether a chat is busy now
    var board: Device?
    var busy: () -> Bool = { false }
    private var boardRetry: DispatchWorkItem?

    // Only a copy in an Applications folder updates itself (not build/Bobble.app while developing)
    private var installed: Bool {
        let parent = Bundle.main.bundleURL.deletingLastPathComponent().path
        return parent == "/Applications" || parent == FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Applications").path
    }

    func start() {
        guard !Updater.repo.isEmpty else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 20) { [weak self] in self?.check() }   // (after launch settles)
        Timer.scheduledTimer(withTimeInterval: 4 * 3600, repeats: true) { [weak self] _ in self?.check() }
    }

    func check() {
        guard !Updater.repo.isEmpty else { status = .failed("No update repo in this build"); return }
        if case .checking = status { return }
        if case .installing = status { return }
        if case .updatingBoard = status { return }
        boardRetry?.cancel()
        status = .checking
        var req = URLRequest(url: URL(string: "https://api.github.com/repos/\(Updater.repo)/releases/latest")!)
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        req.setValue("Bobble/\(Updater.build)", forHTTPHeaderField: "User-Agent")
        URLSession.shared.dataTask(with: req) { [weak self] data, resp, _ in
            DispatchQueue.main.async {
                guard let self else { return }
                if (resp as? HTTPURLResponse)?.statusCode == 404 { self.status = .upToDate; return }   // (no releases yet)
                guard (resp as? HTTPURLResponse)?.statusCode == 200, let data,
                      let j = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
                      let tag = j["tag_name"] as? String, let n = Int(tag.replacingOccurrences(of: "build-", with: "")),
                      let assets = j["assets"] as? [[String: Any]],
                      let url = (assets.first { $0["name"] as? String == "Bobble.zip" }?["browser_download_url"] as? String)
                        .flatMap(URL.init(string:)) else {
                    self.status = .failed("Couldn't reach GitHub")
                    return
                }
                let fw = (assets.first { $0["name"] as? String == "bobble-firmware.bin" }?["browser_download_url"] as? String)
                    .flatMap(URL.init(string:))
                if n > Updater.build {
                    guard self.installed else { self.status = .failed("Build \(n) is out (this copy isn't in Applications)"); return }
                    self.install(url, build: n)      // (the new copy sees to the board once it's running)
                    return
                }
                guard self.installed, let fw else { self.status = .upToDate; return }   // (a copy being developed leaves the board alone)
                self.updateBoard(fw, build: n)
            }
        }.resume()
    }

    // The board's firmware: only a release build older than this release (an everyday build, which says release false,
    // is someone developing on it). It restarts (~10 s), so not while a chat is busy: looked at again each minute.
    private func updateBoard(_ url: URL, build n: Int) {
        guard let board, board.paired else { status = .upToDate; return }
        board.firmware { [weak self] fw in
            guard let self else { return }
            guard let fw, fw.release != false, fw.build < n else { self.status = .upToDate; return }
            if self.busy() {
                self.status = .boardWaiting(n)
                let w = DispatchWorkItem { [weak self] in self?.updateBoard(url, build: n) }
                self.boardRetry = w
                DispatchQueue.main.asyncAfter(deadline: .now() + 60, execute: w)
                return
            }
            self.status = .updatingBoard(n)
            URLSession.shared.dataTask(with: url) { data, resp, _ in
                DispatchQueue.main.async {
                    // (an ESP32 app image starts with 0xE9; the board checks the rest before it switches)
                    guard (resp as? HTTPURLResponse)?.statusCode == 200, let data, data.count > 100_000, data.first == 0xE9 else {
                        self.status = .failed("The display update didn't download")
                        return
                    }
                    board.updateFirmware(data) { code in
                        switch code {
                        case 200: self.status = .boardUpdated(n)
                        case 403: self.status = .failed("Pair with your Bobble again so it can be updated")
                        default: self.status = .failed("Couldn't update your Bobble just now (it'll try again later)")
                        }
                    }
                }
            }.resume()
        }
    }

    private func install(_ url: URL, build n: Int) {
        status = .installing(n)
        URLSession.shared.downloadTask(with: url) { [weak self] file, resp, _ in
            let fm = FileManager.default
            let dir = fm.temporaryDirectory.appendingPathComponent("BobbleUpdate-\(n)")
            var failure: String?
            defer { if let failure { DispatchQueue.main.async { self?.status = .failed(failure) } } }
            guard let file, (resp as? HTTPURLResponse)?.statusCode == 200 else { failure = "The download failed"; return }
            try? fm.removeItem(at: dir)
            try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
            guard Updater.run("/usr/bin/ditto", ["-x", "-k", file.path, dir.path]) else { failure = "Couldn't unzip the update"; return }
            let app = dir.appendingPathComponent("Bobble.app")
            let info = Bundle(url: app)?.infoDictionary
            guard info?["CFBundleIdentifier"] as? String == Updater.bundleID, info?["CFBundleVersion"] as? String == String(n),
                  Updater.run("/usr/bin/codesign", ["--verify", "--deep", app.path]) else { failure = "The download wasn't a Bobble build"; return }
            DispatchQueue.main.async { Updater.swap(in: app) }
        }.resume()
    }

    // Once we've quit: the new copy goes where this one is (the old one back if the move fails), then it starts
    private static func swap(in new: URL) {
        let dest = Bundle.main.bundleURL.path
        let script = """
            while kill -0 \(ProcessInfo.processInfo.processIdentifier) 2>/dev/null; do sleep 0.2; done
            rm -rf "$2.old"; mv "$2" "$2.old" || exit 1
            if mv "$1" "$2"; then rm -rf "$2.old"; else mv "$2.old" "$2"; fi
            xattr -dr com.apple.quarantine "$2" 2>/dev/null
            open "$2"
            """
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", script, "sh", new.path, dest]
        do { try p.run() } catch { NSLog("can't install the update: \(error)"); return }
        NSApp.terminate(nil)
    }

    private static func run(_ tool: String, _ args: [String]) -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return false }
        p.waitUntilExit()
        return p.terminationStatus == 0
    }
}
