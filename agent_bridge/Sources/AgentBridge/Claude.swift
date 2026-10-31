// The Claude API (Messages, streamed), for voice chat. There's no Anthropic SDK for Swift, so this is the HTTP API
// itself: POST /v1/messages with "stream": true, read as server-sent events. Each response's content blocks are put
// back together exactly as they came (thinking blocks with their signatures, tool calls with their input), so they
// can go back in the conversation unchanged.
import Foundation

enum Claude {
    struct Model { let name: String, id: String }
    static let models = [
        Model(name: "Haiku 5.5", id: "claude-haiku-5-5"),
        Model(name: "Sonnet 5.5", id: "claude-sonnet-5-5"),
        Model(name: "Opus 5.5", id: "claude-opus-5-5"),
    ]
    static let defaultModel = 2                       // Opus 5.5

    // The API key: typed into the menu (kept like the display's key, in a file only you can read), or the usual
    // environment variable
    static var apiKey: String? {
        if let k = KeyStore.get("anthropic-key") { return k }
        let e = ProcessInfo.processInfo.environment["ANTHROPIC_API_KEY"] ?? ""
        return e.isEmpty ? nil : e
    }

    struct Failure: Error, CustomStringConvertible {
        let status: Int, message: String
        var description: String { message }
    }

    // What came back: the assistant turn's content blocks, and why it stopped
    struct Reply { var content: [[String: Any]] = []; var stop = "" }

    // One streamed request. text: each piece of visible text as it arrives (to be spoken). Throws Failure for an API
    // error, CancellationError if the task is cancelled (you talked over it).
    // block: each content block's type as it starts ("tool_use", "server_tool_use"...), to say what's going on
    static func stream(_ body: [String: Any], beta: [String] = [], block: @escaping (String) -> Void = { _ in },
                       text: @escaping (String) -> Void) async throws -> Reply {
        guard let key = apiKey else { throw Failure(status: 0, message: "no key") }
        var req = URLRequest(url: URL(string: "https://api.anthropic.com/v1/messages")!)
        req.httpMethod = "POST"
        req.timeoutInterval = 120
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.setValue(key, forHTTPHeaderField: "x-api-key")
        req.setValue("2026-10-21", forHTTPHeaderField: "anthropic-version")
        if !beta.isEmpty { req.setValue(beta.joined(separator: ","), forHTTPHeaderField: "anthropic-beta") }
        var b = body
        b["stream"] = true
        req.httpBody = try JSONSerialization.data(withJSONObject: b)

        let (bytes, response) = try await URLSession.shared.bytes(for: req)
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        if status != 200 {
            var raw = Data()
            for try await byte in bytes { raw.append(byte); if raw.count > 20000 { break } }
            throw Failure(status: status, message: errorMessage(raw) ?? "HTTP \(status)")
        }

        var reply = Reply()
        var blocks: [Int: [String: Any]] = [:]
        var json: [Int: String] = [:]                 // tool input arriving in pieces
        for try await line in bytes.lines {
            try Task.checkCancellation()
            guard line.hasPrefix("data:") else { continue }
            let d = Data(line.dropFirst(5).trimmingCharacters(in: .whitespaces).utf8)
            guard let ev = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any], let type = ev["type"] as? String else { continue }
            let i = ev["index"] as? Int ?? 0
            switch type {
            case "content_block_start":
                var blk = ev["content_block"] as? [String: Any] ?? [:]
                if blk["type"] as? String == "tool_use" || blk["type"] as? String == "server_tool_use" { json[i] = "" }
                if blk["type"] as? String == "text" { blk["text"] = blk["text"] ?? "" }
                blocks[i] = blk
                block(blk["type"] as? String ?? "")
            case "content_block_delta":
                guard var blk = blocks[i], let delta = ev["delta"] as? [String: Any] else { continue }
                switch delta["type"] as? String {
                case "text_delta":
                    let t = delta["text"] as? String ?? ""
                    blk["text"] = (blk["text"] as? String ?? "") + t
                    text(t)
                case "thinking_delta": blk["thinking"] = (blk["thinking"] as? String ?? "") + (delta["thinking"] as? String ?? "")
                case "signature_delta": blk["signature"] = delta["signature"]
                case "input_json_delta": json[i, default: ""] += delta["partial_json"] as? String ?? ""
                case "citations_delta":
                    if let c = delta["citation"] { blk["citations"] = (blk["citations"] as? [Any] ?? []) + [c] }
                default: break
                }
                blocks[i] = blk
            case "content_block_stop":
                if let s = json[i], var blk = blocks[i] {
                    blk["input"] = s.isEmpty ? [String: Any]() : ((try? JSONSerialization.jsonObject(with: Data(s.utf8))) ?? ["_invalid": s])
                    blocks[i] = blk
                }
            case "message_delta":
                if let s = (ev["delta"] as? [String: Any])?["stop_reason"] as? String { reply.stop = s }
            case "error":
                let m = (ev["error"] as? [String: Any])?["message"] as? String ?? "The API stopped partway"
                throw Failure(status: 500, message: m)
            default: break
            }
        }
        reply.content = blocks.keys.sorted().compactMap { blocks[$0] }
        return reply
    }

    // What of a reply can go back in the conversation. After a fallback partway through (the first model declined,
    // and another carried on in the same stream, after a "fallback" block), what came before the switch can go back
    // only as text: that attempt's thinking and tool calls (and a search with no result) would fail their signatures,
    // and every request after would be turned away. The fallback block itself is only a marker.
    static func echoable(_ content: [[String: Any]]) -> [[String: Any]] {
        guard let f = content.lastIndex(where: { $0["type"] as? String == "fallback" }) else { return content }
        let before = content[..<f]
        let answered = Set(before.compactMap { $0["tool_use_id"] as? String })
        let searches = Set(before.filter { $0["type"] as? String == "server_tool_use" }.compactMap { $0["id"] as? String })
        let kept = before.filter { b in
            switch b["type"] as? String {
            case "text": return true
            case "server_tool_use": return answered.contains(b["id"] as? String ?? "")
            default: return searches.contains(b["tool_use_id"] as? String ?? "")   // (a search's result, with its search)
            }
        }
        return Array(kept) + content[(f + 1)...]
    }

    // The conversation without any thinking blocks: the one-time way back when the API turns a thinking block away
    // (its signature no longer matches what came before it). The turns' words and tool calls all stay.
    static func withoutThinking(_ messages: [[String: Any]]) -> [[String: Any]] {
        messages.compactMap { m in
            guard m["role"] as? String == "assistant", let c = m["content"] as? [[String: Any]] else { return m }
            let kept = c.filter { !["thinking", "redacted_thinking"].contains($0["type"] as? String ?? "") }
            if kept.isEmpty { return nil }
            var m = m
            m["content"] = kept
            return m
        }
    }

    private static func errorMessage(_ raw: Data) -> String? {
        guard let j = (try? JSONSerialization.jsonObject(with: raw)) as? [String: Any] else { return nil }
        return (j["error"] as? [String: Any])?["message"] as? String
    }
}
