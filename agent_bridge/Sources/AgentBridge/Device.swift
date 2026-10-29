// The display: found on the local network by its Bonjour service (_bobble._tcp, advertised by the firmware), and
// sent each chat's state with POST /agent. Sends go one at a time, newest state per chat: if the display is away
// for a while, it gets just the latest of each when it's back.
import Foundation
import Network

final class Device {
    enum Status: Equatable { case searching, found(String), online(String), unreachable(String), unpaired(String), several(Int) }
    // A Bobble on the network: its name ("Bobble 3F2A", or what it was named) and id (from its hardware; boards on
    // older firmware have none)
    struct Board: Equatable { let endpoint: NWEndpoint; let name: String; let id: String? }
    private(set) var boards: [Board] = []             // every Bobble seen, for choosing one to pair with

    private(set) var status: Status = .searching { didSet { if status != oldValue { onChange() } } }
    private var browser: NWBrowser?
    private var endpoint: NWEndpoint?
    private(set) var name = ""
    private var pending: [String: Data] = [:]        // chat id -> its newest state, not yet delivered
    private var order: [String] = []                 // ...in the order they came in
    private var sending = false
    private var retry: DispatchWorkItem?
    var onChange: () -> Void = {}
    // The key the display gave us when paired (read once, then kept in memory). A board takes nothing else: the
    // development key (the firmware's OTA token, which every board accepts) went to whichever Bobble was on the
    // network, including one given away. Only the simulator, a development copy on this Mac, still takes it.
    private lazy var pairedKey: String? = sim.map { KeyStore.get($0.keyFile) } ?? KeyStore.get()
    var key: String {
        if let pairedKey { return pairedKey }
        guard simulator else { return "" }
        return UserDefaults.standard.string(forKey: "deviceKey")
            ?? UserDefaults(suiteName: "dev.agentbridge.AgentBridge")?.string(forKey: "deviceKey") ?? ""   // (from when it was Agent Bridge)
    }
    var paired: Bool { pairedKey != nil }
    // The simulators (the firmware running on this Mac, from ~/esp/amoled_demo/sim) are displays of their own: Bobble
    // Bubble (the round board) and Bobble Square (the 4" square one). One Device follows the board, one each simulator,
    // and chats go to all of them. dir: where it keeps its things (its port file), under Application Support.
    struct Sim { let name: String, dir: String, keyFile: String }
    static let bubble = Sim(name: "Bobble Bubble", dir: "BobbleSim", keyFile: "sim-key")
    static let square = Sim(name: "Bobble Square", dir: "BobbleSquare", keyFile: "sim-square-key")
    private static let simNames: Set = ["Bobble Simulator", bubble.name, square.name]
    let sim: Sim?
    var simulator: Bool { sim != nil }
    init(sim: Sim? = nil) { self.sim = sim }

    func start() {
        if simulator { watchSimulator(); return }
        let b = NWBrowser(for: .bonjourWithTXTRecord(type: "_bobble._tcp", domain: nil), using: .tcp)
        b.browseResultsChangedHandler = { [weak self] results, _ in
            guard let self else { return }
            self.boards = results.compactMap { r in
                guard case let .service(name, _, _, _) = r.endpoint, !Device.simNames.contains(name) else { return nil }
                var id: String?
                if case let .bonjour(txt) = r.metadata { id = txt["id"] }
                return Board(endpoint: r.endpoint, name: name, id: id)
            }.sorted { $0.name < $1.name }
            self.follow()
        }
        b.start(queue: .main)
        browser = b
    }

    // Which board is ours: the one we paired with (by its id), or, until we know, the only one about. With several
    // and none of them ours yet, none: pairing picks one.
    var displayID: String? {
        get { UserDefaults.standard.string(forKey: "displayID") }
        set { UserDefaults.standard.set(newValue, forKey: "displayID") }
    }
    private var current: Board?

    private func follow() {
        let ours: Board?
        if let want = displayID { ours = boards.first { $0.id == want } }
        else if boards.count == 1 { ours = boards[0] }
        else { ours = nil }
        guard let b = ours else {
            current = nil
            endpoint = nil
            status = boards.count > 1 && displayID == nil ? .several(boards.count) : .searching
            return
        }
        let changed = current?.endpoint != b.endpoint
        current = b
        endpoint = b.endpoint
        name = b.name
        if changed || status == .searching || { if case .several = status { return true }; return false }() { status = .found(name) }
        pump()
        if spotifyConnected == nil { refreshSpotify() }
    }

    // Queue a chat's state for the display (replacing any older one for that chat that hasn't gone yet)
    func send(chat: String, _ body: [String: Any]) {
        guard let data = try? JSONSerialization.data(withJSONObject: body) else { return }
        if pending[chat] == nil { order.append(chat) }
        pending[chat] = data
        pump()
    }

    // Choices tapped on the display for these chats (ours: waiting on one), as [(session, qid, choice)]
    func fetchAnswers(_ sessions: [String], _ done: @escaping ([(String, String, Int)]) -> Void) {
        guard let endpoint, let body = try? JSONSerialization.data(withJSONObject: ["sessions": sessions]) else { return }
        request(endpoint, "/agent/answers", body, key: key, timeout: 2) { code, reply in
            guard code == 200, let reply, let list = (try? JSONSerialization.jsonObject(with: reply)) as? [[String: Any]] else { return }
            done(list.compactMap { a in
                guard let s = a["session"] as? String, let q = a["qid"] as? String, let c = a["choice"] as? Int else { return nil }
                return (s, q, c)
            })
        }
    }

    private func pump() {
        guard !sending, let endpoint, let chat = order.first, let data = pending[chat] else { return }
        sending = true
        request(endpoint, "/agent", data, key: key) { [weak self] code, _ in
            guard let self else { return }
            self.sending = false
            if code == 403 {                         // the display doesn't know us (any more): pair again
                self.status = .unpaired(self.name)
                return
            }
            if code == 200 {
                self.status = .online(self.name)
                if self.displayID == nil, let id = self.current?.id { self.displayID = id }   // (paired before boards had ids)
                if self.pending[chat] == data {      // (a newer state may have come in meanwhile: that one's next)
                    self.pending[chat] = nil
                    self.order.removeFirst()
                }
                self.pump()
            } else {
                self.status = .unreachable(self.name)
                self.retry?.cancel()                 // try again shortly (newest states only, by then)
                let w = DispatchWorkItem { [weak self] in self?.pump() }
                self.retry = w
                DispatchQueue.main.asyncAfter(deadline: .now() + 3, execute: w)
            }
        }
    }

    // The simulator isn't on the network (no Bonjour): while it's online it leaves its port in a file, and answers on
    // 127.0.0.1. Looked for every few seconds.
    private func watchSimulator() {
        guard let sim else { return }
        let file = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/\(sim.dir)/port")
        Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            guard let self else { return }
            guard let s = try? String(contentsOf: file, encoding: .utf8), let p = UInt16(s.trimmingCharacters(in: .whitespacesAndNewlines)),
                  let port = NWEndpoint.Port(rawValue: p) else { self.lostSimulator(); return }
            let e = NWEndpoint.hostPort(host: "127.0.0.1", port: port)
            self.request(e, "/status", Data(), method: "GET") { code, _ in
                guard code == 200 else { self.lostSimulator(); return }
                if self.endpoint != e { self.endpoint = e; self.name = sim.name }
                if case .searching = self.status { self.status = .found(self.name) }
                self.pump()
                if self.spotifyConnected == nil { self.refreshSpotify() }
            }
        }.fire()
    }

    private func lostSimulator() {
        endpoint = nil
        status = .searching
    }

    // ---- pairing: the display shows a code, you type it here, the display hands over a key of our own

    // On the board chosen (several about), or ours. It shows a code; the code comes back in finishPairing.
    private var pairing: Board?
    func startPairing(_ board: Board? = nil, _ done: @escaping (Bool) -> Void) {
        pairing = board
        guard let e = board?.endpoint ?? endpoint else { done(false); return }
        request(e, "/pair/start", Data("{}".utf8)) { code, _ in done(code == 200) }
    }

    // Changed our mind (the code dialog was cancelled): the board takes its code down
    func cancelPairing() {
        guard let e = pairing?.endpoint ?? endpoint else { return }
        pairing = nil
        request(e, "/pair/cancel", Data("{}".utf8)) { _, _ in }
    }

    // Who this Mac is, for the board's list of paired Macs (pairing again replaces this Mac's old key, not another's)
    private var clientID: String {
        if let c = UserDefaults.standard.string(forKey: "clientID") { return c }
        let c = (0..<8).map { _ in String(format: "%02x", UInt8.random(in: 0...255)) }.joined()
        UserDefaults.standard.set(c, forKey: "clientID")
        return c
    }

    enum PairResult { case paired, wrongCode, expired, failed }
    func finishPairing(code: String, _ done: @escaping (PairResult) -> Void) {
        let mac = Host.current().localizedName ?? "Mac"
        guard let e = pairing?.endpoint ?? endpoint,
              let body = try? JSONSerialization.data(withJSONObject: ["code": code, "client": clientID, "mac": mac]) else { done(.failed); return }
        request(e, "/pair/finish", body) { [weak self] status, reply in
            switch status {
            case 200:
                guard let reply, let j = (try? JSONSerialization.jsonObject(with: reply)) as? [String: Any],
                      let key = j["key"] as? String, !key.isEmpty else { done(.failed); return }
                KeyStore.set(key, file: self?.sim?.keyFile ?? "display-key")
                self?.pairedKey = key
                if self?.simulator == false {         // this board is ours now (its id; older firmware: the one we picked)
                    self?.displayID = j["id"] as? String ?? self?.pairing?.id
                    self?.follow()
                }
                self?.pairing = nil
                self?.status = .found(self?.name ?? "")
                self?.pump()                         // (anything that was waiting goes now)
                done(.paired)
            case 403: done(.wrongCode)
            case 410: done(.expired)
            default: done(.failed)
            }
        }
    }

    // ---- its name: "Bobble 3F2A" until it's renamed here (it tells the network the new name itself)

    func rename(_ newName: String, _ done: @escaping (String?) -> Void) {
        guard let endpoint, let body = try? JSONSerialization.data(withJSONObject: ["name": newName]) else { done(nil); return }
        request(endpoint, "/name", body, key: key) { [weak self] code, reply in
            guard code == 200, let reply, let j = (try? JSONSerialization.jsonObject(with: reply)) as? [String: Any],
                  let n = j["name"] as? String else { done(nil); return }
            self?.name = n
            if let s = self?.status, case .online = s { self?.status = .online(n) }
            done(n)
        }
    }

    // ---- firmware: which build the board runs, and putting a new one on it (Updater does this from a release)

    // Its release build number (0: an everyday build, or firmware from before boards said), and whether it's a release
    // build (nil: too old to say). nil overall: not reachable.
    func firmware(_ done: @escaping ((build: Int, release: Bool?)?) -> Void) {
        guard !simulator, let endpoint else { done(nil); return }
        request(endpoint, "/info", Data(), method: "GET") { code, body in
            guard code == 200, let body, let j = (try? JSONSerialization.jsonObject(with: body)) as? [String: Any] else { done(nil); return }
            done(((j["fw"] as? NSNumber)?.intValue ?? 0, j["release"] as? Bool))
        }
    }

    // POST /ota with our key: the board checks the image, switches to it and restarts (and goes back to the old one by
    // itself if the new one doesn't come up). Calls back with the status code: 403 not paired, 0 not reachable.
    func updateFirmware(_ image: Data, _ done: @escaping (Int) -> Void) {
        guard !simulator, paired, let endpoint else { done(0); return }
        request(endpoint, "/ota", image, key: key, timeout: 180) { code, _ in done(code) }
    }

    // ---- voice chat (the square board, and its simulator): where to connect for it (VoiceChat's ChatLink)

    // The board's address and its chat port (GET /info says it, if the board can talk), or nil: can't talk, or away
    func chatEndpoint(_ done: @escaping (NWEndpoint?) -> Void) {
        guard let endpoint else { done(nil); return }
        request(endpoint, "/info", Data(), method: "GET", host: { [weak self] in self?.lastHost = $0 }) { [weak self] code, body in
            guard code == 200, let body, let j = (try? JSONSerialization.jsonObject(with: body)) as? [String: Any],
                  let port = (j["chat"] as? NSNumber)?.uint16Value, port > 0, let p = NWEndpoint.Port(rawValue: port),
                  let host = self?.lastHost else { done(nil); return }
            done(.hostPort(host: host, port: p))
        }
    }
    private var lastHost: NWEndpoint.Host?

    // ---- calendars: kept on the display (their private .ics links); this just shows and replaces the list

    func getCalendars(_ done: @escaping ([String]?) -> Void) {
        guard let endpoint else { done(nil); return }
        request(endpoint, "/calendars", Data(), key: key, method: "GET") { code, body in
            guard code == 200, let body, let j = (try? JSONSerialization.jsonObject(with: body)) as? [String: Any] else { done(nil); return }
            done(j["urls"] as? [String] ?? [])
        }
    }

    func setCalendars(_ urls: [String], _ done: @escaping (Int?) -> Void) {
        guard let endpoint, let body = try? JSONSerialization.data(withJSONObject: ["urls": urls]) else { done(nil); return }
        request(endpoint, "/calendars", body, key: key) { code, reply in
            guard code == 200, let reply, let j = (try? JSONSerialization.jsonObject(with: reply)) as? [String: Any] else { done(nil); return }
            done(j["count"] as? Int)
        }
    }

    // ---- Spotify: the display keeps the sign-in (a refresh token) and talks to Spotify itself

    private(set) var spotifyConnected: Bool?          // as the display last said (nil: not asked yet)

    func refreshSpotify() {
        guard let endpoint else { return }
        request(endpoint, "/spotify", Data(), key: key, method: "GET") { [weak self] code, body in
            guard code == 200, let body, let j = (try? JSONSerialization.jsonObject(with: body)) as? [String: Any] else { return }
            self?.spotifyConnected = j["connected"] as? Bool
        }
    }

    func setSpotify(clientID: String, refreshToken: String, _ done: @escaping (Bool) -> Void) {
        guard let endpoint, let body = try? JSONSerialization.data(withJSONObject: ["client_id": clientID, "refresh_token": refreshToken])
        else { done(false); return }
        request(endpoint, "/spotify", body, key: key) { [weak self] code, _ in
            if code == 200 { self?.spotifyConnected = true }
            done(code == 200)
        }
    }

    func disconnectSpotify(_ done: @escaping (Bool) -> Void) {
        guard let endpoint else { done(false); return }
        request(endpoint, "/spotify", Data(), key: key, method: "DELETE") { [weak self] code, _ in
            if code == 200 { self?.spotifyConnected = false }
            done(code == 200)
        }
    }

    // One HTTP request to the display: calls back with the status code (0 if it couldn't be reached) and the body
    // host: told the address it reached (a Bonjour name resolves to one), for connecting to the board on another port
    private func request(_ endpoint: NWEndpoint, _ path: String, _ body: Data, key: String = "", method: String = "POST",
                         timeout: Double = 5, host: ((NWEndpoint.Host) -> Void)? = nil, _ done: @escaping (Int, Data?) -> Void) {
        let c = NWConnection(to: endpoint, using: .tcp)
        var finished = false
        func finish(_ code: Int, _ data: Data?) {
            if finished { return }
            finished = true
            c.cancel()
            done(code, data)
        }
        c.stateUpdateHandler = { state in
            switch state {
            case .ready:
                if let host, case let .hostPort(h, _)? = c.currentPath?.remoteEndpoint { host(h) }
                var req = "\(method) \(path) HTTP/1.1\r\nHost: bobble\r\nContent-Type: application/json\r\n"
                if !key.isEmpty { req += "X-OTA-Token: \(key)\r\n" }
                req += "Content-Length: \(body.count)\r\nConnection: close\r\n\r\n"
                c.send(content: Data(req.utf8) + body, completion: .contentProcessed { _ in })
                HTTP.readMessage(c) { reply in
                    let code = reply.flatMap { Int($0.first.split(separator: " ").dropFirst().first ?? "") } ?? 0
                    finish(code, reply?.body)
                }
            case .failed, .cancelled, .waiting:
                finish(0, nil)                       // (no route: the display's off or on another network)
            default: break
            }
        }
        c.start(queue: .main)
        DispatchQueue.main.asyncAfter(deadline: .now() + timeout) { finish(0, nil) }
    }
}

// The display's key, in a file only this user can read (~/Library/Application Support/Bobble/display-key).
// (The Keychain would be the place, but it ties an item to the app's signature, and this ad-hoc-signed build gets a
// new one each time it's rebuilt, so macOS asks for your password again. Back to the Keychain once it's signed properly.)
enum KeyStore {
    static var url: URL { support.appendingPathComponent("Bobble/display-key") }
    private static var support: URL { FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0] }

    static func get(_ file: String = "display-key") -> String? {
        let url = support.appendingPathComponent("Bobble/\(file)")
        let old = support.appendingPathComponent("Agent Bridge/display-key")   // (from when it was Agent Bridge)
        guard let s = (try? String(contentsOf: url, encoding: .utf8)) ?? (file == "display-key" ? try? String(contentsOf: old, encoding: .utf8) : nil)
        else { return nil }
        let k = s.trimmingCharacters(in: .whitespacesAndNewlines)
        return k.isEmpty ? nil : k
    }

    static func set(_ key: String, file: String = "display-key") {
        let url = support.appendingPathComponent("Bobble/\(file)")
        let fm = FileManager.default
        try? fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true,
                                attributes: [.posixPermissions: 0o700])
        fm.createFile(atPath: url.path, contents: Data(key.utf8), attributes: [.posixPermissions: 0o600])
    }
}
