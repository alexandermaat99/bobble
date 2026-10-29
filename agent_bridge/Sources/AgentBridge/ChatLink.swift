// The connection to a board that can talk (the square one, or its simulator), for voice chat: kept open while the
// board's about, so a tap on its mic starts a chat at once. Each message: a type byte ('A' audio, 'J' JSON), a 3-byte
// big-endian length, the bytes (the firmware's chat.h has the whole conversation). We send our paired key first.
import Foundation
import Network

final class ChatLink {
    let device: Device
    private var conn: NWConnection?
    private var ready = false
    private var connecting = false
    private var buffer = Data()
    var onJSON: ([String: Any]) -> Void = { _ in }
    var onAudio: (Data) -> Void = { _ in }
    var onChange: () -> Void = {}
    private(set) var connected = false { didSet { if connected != oldValue { onChange() } } }

    init(device: Device) {
        self.device = device
        // look for the board (and stay connected): every few seconds while we're not; a word every 10 s while we are,
        // so it knows we're still here
        // (in the common modes: they keep going while the menu's open)
        RunLoop.main.add(Timer(timeInterval: 4, repeats: true) { [weak self] _ in self?.tick() }, forMode: .common)
        RunLoop.main.add(Timer(timeInterval: 10, repeats: true) { [weak self] _ in
            if self?.connected == true { self?.send(["t": "ping"]) }
        }, forMode: .common)
    }

    private func tick() {
        guard conn == nil, !connecting, device.paired || device.simulator else { return }
        switch device.status {
        case .found, .online: break
        default: return
        }
        connecting = true
        device.chatEndpoint { [weak self] e in
            guard let self else { return }
            self.connecting = false
            guard let e, self.conn == nil else { return }
            self.connect(e)
        }
    }

    private func connect(_ e: NWEndpoint) {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let c = NWConnection(to: e, using: NWParameters(tls: nil, tcp: tcp))
        conn = c
        c.stateUpdateHandler = { [weak self, weak c] state in
            guard let self, let c, self.conn === c else { return }
            switch state {
            case .ready:
                self.ready = true
                self.send(["t": "hello", "key": self.device.key])
                self.connected = true
                self.receive(c)
            case .failed, .cancelled, .waiting:
                self.drop()
            default: break
            }
        }
        c.start(queue: .main)
    }

    private func drop() {
        conn?.cancel()
        conn = nil
        ready = false
        buffer.removeAll()
        connected = false
    }

    private func receive(_ c: NWConnection) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self, weak c] data, _, done, error in
            guard let self, let c, self.conn === c else { return }
            if let data { self.buffer.append(data); self.parse() }
            if done || error != nil { self.drop(); return }
            self.receive(c)
        }
    }

    private func parse() {
        while buffer.count >= 4 {
            let b = [UInt8](buffer.prefix(4))
            let n = Int(b[1]) << 16 | Int(b[2]) << 8 | Int(b[3])
            guard buffer.count >= 4 + n else { return }
            let body = buffer.subdata(in: buffer.startIndex + 4 ..< buffer.startIndex + 4 + n)
            buffer.removeFirst(4 + n)
            if b[0] == UInt8(ascii: "A") { onAudio(body) }
            else if b[0] == UInt8(ascii: "J"), let j = (try? JSONSerialization.jsonObject(with: body)) as? [String: Any] { onJSON(j) }
        }
    }

    private func frame(_ type: Character, _ body: Data) {
        guard ready, let conn else { return }
        let n = body.count
        var d = Data([type.asciiValue!, UInt8((n >> 16) & 0xFF), UInt8((n >> 8) & 0xFF), UInt8(n & 0xFF)])
        d.append(body)
        conn.send(content: d, completion: .contentProcessed { _ in })
    }

    func send(_ j: [String: Any]) {
        guard let d = try? JSONSerialization.data(withJSONObject: j) else { return }
        frame("J", d)
    }

    // Speech for the speaker: 16 kHz 16-bit mono (in pieces small enough for the board's buffer)
    func sendAudio(_ pcm: Data) {
        var i = pcm.startIndex
        while i < pcm.endIndex {
            let j = min(i + 16000, pcm.endIndex)
            frame("A", pcm.subdata(in: i ..< j))
            i = j
        }
    }
}
