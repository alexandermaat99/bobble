// The hooks' way in: a tiny HTTP server on 127.0.0.1 (this Mac only). Each agent's hook command POSTs the event's
// JSON (Claude Code to /hook, Cursor to /cursor); we answer at once (a hook mustn't hold the agent up) and hand it on.
import Foundation
import Network

final class HookServer {
    static let port: UInt16 = 47823
    private var listener: NWListener?
    private let onEvent: (_ path: String, _ event: [String: Any]) -> Void

    init(onEvent: @escaping (String, [String: Any]) -> Void) {
        self.onEvent = onEvent
    }

    func start() throws {
        let params = NWParameters.tcp
        params.requiredLocalEndpoint = NWEndpoint.hostPort(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: Self.port)!)
        params.allowLocalEndpointReuse = true
        let l = try NWListener(using: params)
        l.newConnectionHandler = { [weak self] c in self?.serve(c) }
        l.stateUpdateHandler = { state in
            if case .failed(let e) = state { NSLog("hook server failed: \(e)") }
        }
        l.start(queue: .main)
        listener = l
    }

    private func serve(_ c: NWConnection) {
        c.start(queue: .main)
        HTTP.readMessage(c) { [weak self] req in
            let reply = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nok\n"
            c.send(content: reply.data(using: .utf8), completion: .contentProcessed { _ in c.cancel() })
            guard let req, let j = (try? JSONSerialization.jsonObject(with: req.body)) as? [String: Any] else { return }
            let path = req.first.split(separator: " ").dropFirst().first.map(String.init) ?? "/hook"   // "POST /cursor HTTP/1.1"
            self?.onEvent(path, j)
        }
    }
}

// Just enough HTTP/1.1 for small requests and replies: headers up to the blank line, then Content-Length bytes
enum HTTP {
    // Reads one request or response: its first line ("HTTP/1.1 200 OK", "POST /hook HTTP/1.1") and body
    static func readMessage(_ c: NWConnection, _ done: @escaping ((first: String, body: Data)?) -> Void) {
        var buf = Data()
        func more() {
            c.receive(minimumIncompleteLength: 1, maximumLength: 65536) { data, _, isComplete, error in
                if let data { buf.append(data) }
                if let end = buf.range(of: Data("\r\n\r\n".utf8)) {
                    let head = String(decoding: buf[..<end.lowerBound], as: UTF8.self)
                    let len = head.split(separator: "\r\n").compactMap { line -> Int? in
                        let kv = line.split(separator: ":", maxSplits: 1)
                        guard kv.count == 2, kv[0].lowercased() == "content-length" else { return nil }
                        return Int(kv[1].trimmingCharacters(in: .whitespaces))
                    }.first ?? 0
                    let body = buf[end.upperBound...]
                    let first = head.split(separator: "\r\n").first.map(String.init) ?? ""
                    if body.count >= len { done((first, Data(body.prefix(len)))); return }
                }
                if isComplete || error != nil { done(nil); return }
                more()
            }
        }
        more()
    }
}
