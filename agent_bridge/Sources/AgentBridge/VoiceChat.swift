// Talking with Claude through the square Bobble: its microphones in, Claude's voice out. The board (chat.c) does the
// buttons, the sound and the echo cancelling; this does the rest: speech to text (Ears), Claude (Claude.swift, with
// tools to act on this Mac), and text to speech (Mouth), sent back to the board a little ahead of where it's playing.
//
// You can talk over Claude: what the board hears while Claude speaks is checked against what Claude is saying (what's
// left of its own voice after the echo cancelling would match), and new words stop it there. Claude's turn is kept
// in the conversation as far as it got, marked as interrupted.
import AppKit

final class VoiceChat {
    private let chats: Chats
    private var links: [ChatLink] = []
    private var active: ChatLink?                     // the board the chat is on
    private let ears = Ears()
    private let mouth = Mouth()

    private var messages: [[String: Any]] = []        // the conversation, as the API takes it (only ever added to)
    private var system = ""                           // fixed for the conversation (so the cache and thinking hold)
    private var model: Int {
        get { let m = UserDefaults.standard.object(forKey: "chatModel") as? Int ?? Claude.defaultModel; return Claude.models.indices.contains(m) ? m : Claude.defaultModel }
        set { UserDefaults.standard.set(newValue, forKey: "chatModel") }
    }

    private enum State: String { case idle, listening, thinking, speaking }
    private var state = State.idle
    private var turn: Task<Void, Never>?              // Claude answering (cancelled if you talk over it)
    private var spoken = ""                           // what Claude has said aloud so far this turn
    private var pendingText = ""                      // streamed text not yet a whole sentence
    private var recent: [String] = []                 // the last sentences shown
    private var ending = false                        // Claude said goodbye: close once it's been said
    private var turnID = 0                            // (streamed text from a turn that's been stopped is dropped)
    private var drained = true                        // the board has played everything it was sent
    private var cutNote = ""                          // Claude was cut off: said with your next words

    // Speech going out: sent no more than a couple of seconds ahead of the board's playing, so stopping is quick
    private var outgoing = Data()
    private var playStart: Date?                      // when the board started on this stretch of speech
    private var sentSeconds = 0.0
    private var captions: [(at: Double, text: String)] = []
    private var pacer: Timer?
    private static let ahead = 2.0

    private var asks: [Int: (String) -> Void] = [:]   // questions to the board awaiting its reply
    private var askID = 0

    init(chats: Chats, devices: [Device]) {
        self.chats = chats
        for d in devices {
            let l = ChatLink(device: d)
            l.onJSON = { [weak self, weak l] j in if let self, let l { self.received(j, from: l) } }
            l.onAudio = { [weak self, weak l] pcm in if let l, self?.active === l { self?.ears.feed(pcm) } }
            l.onChange = { [weak self, weak l] in   // (the board went away mid-chat: it closes its side itself)
                if let self, let l, !l.connected, self.active === l { self.finish() }
            }
            links.append(l)
        }
        ears.onPartial = { [weak self] t in self?.heardSoFar(t) }
        ears.onUtterance = { [weak self] t in self?.utterance(t) }
        mouth.onAudio = { [weak self] pcm, sentence in self?.queueSpeech(pcm, sentence) }
        pacer = Timer(timeInterval: 0.05, repeats: true) { [weak self] _ in self?.pace() }
        RunLoop.main.add(pacer!, forMode: .common)    // (keeps going while the menu's open)
    }

    var connected: Bool { links.contains { $0.connected } }

    // ---------------------------------------------------------------- the board

    private func received(_ j: [String: Any], from link: ChatLink) {
        switch j["t"] as? String ?? "" {
        case "ready":
            sendModels(link)
            if j["active"] as? Bool == true, active == nil || active === link { begin(on: link) }   // (it was mid-chat)
        case "start": begin(on: link)
        case "stop": if link === active { finish() }
        case "mute":
            guard link === active else { return }
            if j["on"] as? Bool == true {
                // a tap on the mic just after you've spoken: that's you finished, so it's sent now rather than lost
                let said = ears.heard.trimmingCharacters(in: .whitespaces)
                ears.stop()
                if state == .listening, !said.isEmpty { NSLog("voice chat: sent on mute"); utterance(said) }
            } else if state != .idle { ears.start() }
        case "model":
            if let i = j["i"] as? Int, Claude.models.indices.contains(i) { model = i; for l in links { sendModels(l) } }
        case "new":
            interrupt(record: false)
            messages.removeAll()
            system = VoiceChat.systemPrompt()
            if link === active { set(.listening); link.send(["t": "heard", "text": ""]); link.send(["t": "say", "text": ""]) }
        case "played":
            guard link === active else { return }
            // (sent as the board ran dry: if more has gone since, that's still playing, and this isn't about it)
            if let start = playStart, Date() < start.addingTimeInterval(sentSeconds - 0.3) { return }
            drained = true
            if state == .speaking, turn == nil, mouth.idle, outgoing.isEmpty { speechDone() }
        case "reply":
            if let id = j["id"] as? Int, let f = asks.removeValue(forKey: id) { f(j["text"] as? String ?? "") }
        default: break
        }
    }

    private func sendModels(_ l: ChatLink) {
        l.send(["t": "models", "names": Claude.models.map(\.name), "current": model])
    }

    private func send(_ j: [String: Any]) { active?.send(j) }

    private func status(_ s: String) { send(["t": "status", "text": s]) }

    private func set(_ s: State) {
        state = s
        send(["t": "state", "s": s.rawValue])
    }

    // A chat starts (the mic was tapped): listening
    private func begin(on link: ChatLink) {
        NSLog("voice chat: started on %@", link.device.name)
        if let a = active, a !== link { a.send(["t": "end"]) }   // (one at a time)
        mouth.voice = Mouth.bestVoice()               // (looked for each time: a voice downloaded since is used straight away)
        if let v = mouth.voice { NSLog("voice chat: speaking as %@ (%@)", v.name, v.identifier) }
        if turn != nil || !mouth.idle || !outgoing.isEmpty { interrupt(record: true) }
        active = link
        sendModels(link)
        guard Claude.apiKey != nil else {
            status("Add your Claude API key in the Bobble menu on your Mac")
            return
        }
        guard Ears.allowed else {
            Ears.askPermission { [weak self] ok in
                guard let self, self.active === link else { return }
                if ok { self.begin(on: link) } else { self.status("Allow Speech Recognition for Bobble in System Settings") }
            }
            return
        }
        status("")
        ending = false
        if system.isEmpty { system = VoiceChat.systemPrompt() }
        ears.start()
        set(.listening)
    }

    // The chat closed on the board (or Claude said goodbye): everything stops; the conversation stays for next time
    private func finish() {
        interrupt(record: true)
        ears.stop()
        state = .idle
        active = nil
    }

    // ---------------------------------------------------------------- you

    private func heardSoFar(_ t: String) {
        switch state {
        case .speaking, .thinking:
            // Claude's own voice coming back (what the echo cancelling left) matches what it's saying: only words it
            // hasn't said count as you talking over it
            let mine = VoiceChat.words(spoken + " " + pendingText + " " + captions.map(\.text).joined(separator: " "))
            let fresh = VoiceChat.words(t).filter { !mine.contains($0) }
            if fresh.count >= 2 || (state == .thinking && !VoiceChat.words(t).isEmpty) {
                interrupt(record: true)
                set(.listening)
                send(["t": "heard", "text": t])
            }
        case .listening:
            send(["t": "heard", "text": t])
        case .idle: break
        }
    }

    private func utterance(_ t: String) {
        guard state == .listening else { return }      // (heard while Claude was talking and not new: its echo)
        NSLog("voice chat: you said %@", t)
        send(["t": "heard", "text": t])
        send(["t": "say", "text": ""])
        addUser([["type": "text", "text": cutNote.isEmpty ? t : cutNote + "\n\n" + t]])
        cutNote = ""
        respond()
    }

    // Your words go in as a user turn; if the last turn was yours too (Claude never got a word out), they join it
    private func addUser(_ content: [[String: Any]]) {
        if var last = messages.last, last["role"] as? String == "user", var c = last["content"] as? [[String: Any]] {
            c += content
            last["content"] = c
            messages[messages.count - 1] = last
        } else {
            messages.append(["role": "user", "content": content])
        }
    }

    // ---------------------------------------------------------------- Claude

    private func respond() {
        set(.thinking)
        spoken = ""
        recent = []
        pendingText = ""
        let modelIndex = model
        turnID += 1
        let id = turnID
        turn = Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                for _ in 0..<8 {                      // (a few rounds of tools at most)
                    let m = Claude.models[modelIndex]
                    var body: [String: Any] = [
                        "model": m.id,
                        "max_tokens": 4096,
                        "system": [["type": "text", "text": self.system, "cache_control": ["type": "ephemeral"]]],
                        "tools": VoiceTools.definitions(model: m.id),
                        "output_config": ["effort": "low"],
                        "messages": self.messages,
                    ]
                    var beta: [String] = []
                    if m.id != "claude-haiku-5-5" { body["fallbacks"] = "default"; beta.append("server-side-fallback-2026-07-01") }
                    let reply = try await Claude.stream(body, beta: beta, block: { [weak self] type in
                        DispatchQueue.main.async { if self?.turnID == id, type == "server_tool_use" { self?.status("Searching the web") } }
                    }) { [weak self] t in
                        DispatchQueue.main.async { if self?.turnID == id { self?.streamed(t) } }
                    }
                    try Task.checkCancellation()
                    if reply.content.isEmpty { break }    // (nothing more to say, e.g. after end_chat: nothing to keep)
                    // (a paused turn carries on in the same assistant message: its blocks are added to it)
                    if var last = self.messages.last, last["role"] as? String == "assistant", var c = last["content"] as? [[String: Any]] {
                        c += reply.content
                        last["content"] = c
                        self.messages[self.messages.count - 1] = last
                    } else {
                        self.messages.append(["role": "assistant", "content": reply.content])
                    }
                    if reply.stop == "refusal" { self.speak("Sorry, I can't help with that one.") }
                    let calls = reply.content.filter { $0["type"] as? String == "tool_use" }
                    if calls.isEmpty {
                        if reply.stop == "pause_turn" { continue }   // (a web search still going: carry on)
                        break
                    }
                    // every tool call gets its result, whatever happened (or the conversation can't go on)
                    self.flushText()
                    var results: [[String: Any]] = []
                    for c in calls {
                        var out = VoiceTools.Output(text: "Not run: your reply was cut off before the call was complete.", error: true)
                        if reply.stop == "tool_use", let input = c["input"] as? [String: Any], input["_invalid"] == nil {
                            out = await VoiceTools.run(c["name"] as? String ?? "", input, chat: self)
                            try Task.checkCancellation()
                        }
                        var r: [String: Any] = ["type": "tool_result", "tool_use_id": c["id"] as? String ?? "", "content": out.text]
                        if out.error { r["is_error"] = true }
                        results.append(r)
                    }
                    self.messages.append(["role": "user", "content": results])
                    if reply.stop != "tool_use" { break }
                }
                // (the last of the text: after the streamed pieces still on their way to the main thread)
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.turnID == id else { return }
                    self.flushText()
                    self.turn = nil
                    if self.mouth.idle, self.outgoing.isEmpty, self.state == .thinking || self.drained { self.speechDone() }
                }
            } catch is CancellationError {
            } catch let e as Claude.Failure {
                guard self.turnID == id else { return }
                self.turn = nil
                self.failed(e)
            } catch {
                guard self.turnID == id, (error as? URLError)?.code != .cancelled else { return }
                self.turn = nil
                self.status("Couldn't reach Claude")
                self.set(.listening)
            }
        }
    }

    private func failed(_ e: Claude.Failure) {
        NSLog("voice chat: Claude said \(e.status): \(e.message)")
        // (your words stay in: whatever you say next joins them)
        switch e.status {
        case 401, 403: status("The Claude API key didn't work: check it in the Bobble menu")
        case 429: status("Claude is busy (rate limited): try again in a moment")
        case 529, 500...599: status("Claude is overloaded: try again in a moment")
        default: status(String(e.message.prefix(90)))
        }
        set(.listening)
    }

    // Visible text as it streams: spoken a sentence at a time
    private func streamed(_ t: String) {
        pendingText += t
        while let r = VoiceChat.sentenceEnd(pendingText) {
            let s = String(pendingText[..<r])
            pendingText = String(pendingText[r...])
            speak(s)
        }
    }

    private func flushText() {
        let s = pendingText
        pendingText = ""
        speak(s)
    }

    private func speak(_ s: String) {
        let t = VoiceChat.speakable(s)
        guard !t.isEmpty else { return }
        mouth.say(t)
    }

    // ---------------------------------------------------------------- speech out

    private func queueSpeech(_ pcm: Data, _ sentence: String?) {
        guard active != nil else { return }
        if let sentence { captions.append((sentSeconds + Double(outgoing.count) / 32000, sentence)) }
        outgoing.append(pcm)
        drained = false
        if state != .speaking { set(.speaking) }
    }

    private func pace() {
        guard let link = active else { return }
        if !outgoing.isEmpty {
            if playStart == nil { playStart = Date(); sentSeconds = 0 }
            let elapsed = Date().timeIntervalSince(playStart!)
            if sentSeconds < elapsed { playStart = Date().addingTimeInterval(-sentSeconds) }   // (a gap: the board ran dry)
            let room = Int((Date().timeIntervalSince(playStart!) + VoiceChat.ahead - sentSeconds) * 16000) * 2
            if room > 0 {
                let chunk = outgoing.prefix(room)
                link.sendAudio(Data(chunk))
                outgoing.removeFirst(chunk.count)
                sentSeconds += Double(chunk.count) / 32000
            }
        }
        // captions: each sentence shows as it starts playing
        if let start = playStart {
            let now = Date().timeIntervalSince(start)
            while let c = captions.first, c.at <= now + 0.05 {
                captions.removeFirst()
                spoken += (spoken.isEmpty ? "" : " ") + c.text
                recent = Array((recent + [c.text]).suffix(2))
                link.send(["t": "say", "text": recent.joined(separator: " ")])   // (the last sentence or two: the screen holds three lines)
            }
        }
    }

    // Claude has finished, and all of it has played: your turn (or, after a goodbye, the end)
    private func speechDone() {
        playStart = nil
        sentSeconds = 0
        if !captions.isEmpty {
            spoken += " " + captions.map(\.text).joined(separator: " ")
            recent = Array((recent + captions.map(\.text)).suffix(2))
            captions.removeAll()
            send(["t": "say", "text": recent.joined(separator: " ")])
        }
        if ending { send(["t": "end"]); finish(); return }
        // what was heard meanwhile was Claude's own voice, unless it has words Claude didn't say (a quick "yes" over
        // the end of it): those stay, and become your turn when you pause
        let mine = VoiceChat.words(spoken)
        if VoiceChat.words(ears.heard).allSatisfy({ mine.contains($0) }) { ears.reset() }
        set(.listening)
    }

    // Stop talking (you talked over it, or the chat closed): what was said aloud stays in the conversation
    private func interrupt(record: Bool) {
        let wasTalking = turn != nil || !mouth.idle || !outgoing.isEmpty || !captions.isEmpty
        ending = false                                // (talked over the goodbye: still going)
        turnID += 1
        turn?.cancel()
        turn = nil
        mouth.stop()
        let said = (spoken + " " + captions.filter { $0.at <= playedSeconds }.map(\.text).joined(separator: " ")).trimmingCharacters(in: .whitespaces)
        outgoing.removeAll()
        captions.removeAll()
        pendingText = ""
        playStart = nil
        sentSeconds = 0
        drained = true
        spoken = ""
        send(["t": "flush"])
        guard record, wasTalking else { return }
        // The conversation only ever grows. A whole assistant turn is in already (its blocks as they came): if it was
        // cut off while being read out, your next words say so. Tool calls it was waiting on get results, so it stays
        // valid. A turn still streaming isn't in yet: what was heard of it goes in, as plain text.
        let cut = said.isEmpty ? "[You were interrupted before saying anything.]" : "[You were interrupted after saying: \"\(said)\"]"
        if let last = messages.last, last["role"] as? String == "assistant" {
            let calls = (last["content"] as? [[String: Any]] ?? []).filter { $0["type"] as? String == "tool_use" }
            if !calls.isEmpty {
                messages.append(["role": "user", "content": calls.map {
                    ["type": "tool_result", "tool_use_id": $0["id"] as? String ?? "", "content": "Not done: the user interrupted.", "is_error": true]
                }])
            }
            cutNote = cut
        } else if !said.isEmpty {
            messages.append(["role": "assistant", "content": [["type": "text", "text": said + " [interrupted]"]]])
        }
    }

    // How far into this stretch of speech the board has played (roughly: it plays what it's sent, in time)
    private var playedSeconds: Double { playStart.map { min(Date().timeIntervalSince($0), sentSeconds) } ?? 0 }

    // ---------------------------------------------------------------- for the tools

    func goodbye() { ending = true }

    // A question for the board (it has the calendar and Spotify): its answer, or nil if it doesn't say in time
    @MainActor func askBoard(_ q: String, _ a: String? = nil) async -> String? {
        guard let link = active else { return nil }
        askID += 1
        let id = askID
        return await withCheckedContinuation { (k: CheckedContinuation<String?, Never>) in
            var done = false
            asks[id] = { t in if !done { done = true; k.resume(returning: t) } }
            var j: [String: Any] = ["t": "ask", "id": id, "q": q]
            if let a { j["a"] = a }
            link.send(j)
            DispatchQueue.main.asyncAfter(deadline: .now() + 6) { [weak self] in
                self?.asks[id] = nil
                if !done { done = true; k.resume(returning: nil) }
            }
        }
    }

    func doing(_ s: String) { status(s) }

    var agentChats: Chats { chats }

    // ---------------------------------------------------------------- text

    static func systemPrompt() -> String {
        let f = DateFormatter()
        f.dateFormat = "EEEE, MMMM d, yyyy"
        let user = NSFullUserName().split(separator: " ").first.map(String.init) ?? ""
        return """
        You're Claude, talking out loud with \(user.isEmpty ? "the user" : user) through Bobble, a little display with a \
        speaker and microphones that sits on their desk next to their Mac. Everything you write is read out by a \
        text-to-speech voice, so talk the way you would in a conversation: usually one to three short sentences, no \
        markdown, lists, headings, code, emoji or links, and say numbers, times and symbols the way you'd speak them. \
        What they say comes through speech recognition and can have mistakes: go with what they most likely meant, and \
        ask when it's genuinely unclear. They can talk over you; a reply of yours that ends in [interrupted] was cut off \
        there, so don't repeat it, just carry on from what they said.

        You can do things for them with your tools: see what their coding agents are up to (Claude Code, Codex and \
        Cursor chats on this Mac) and answer the ones waiting on a question, start a new Claude Code task in one of \
        their project folders (it opens in Terminal), read their calendar, control their Spotify, and search the web. \
        Before starting a coding task, say back what you'll ask for and in which folder, unless they were already \
        clear about both. When something takes a moment, say a few words first. When they say goodbye or that \
        they're done, say a short goodbye and call end_chat.

        Today is \(f.string(from: Date())).
        """
    }

    // Where a whole sentence ends in streamed text (and the next begins), if one does yet
    private static let abbreviations: Set<String> = ["mr", "mrs", "ms", "dr", "st", "vs", "etc", "e.g", "i.e", "jr", "sr", "no", "approx"]
    static func sentenceEnd(_ s: String) -> String.Index? {
        var i = s.startIndex
        while i < s.endIndex {
            let c = s[i]
            let n = s.index(after: i)
            if c == "\n" { return n }
            if ".!?".contains(c), n < s.endIndex, s[n] == " " || s[n] == "\n" {
                let before = (s[..<i].split(separator: " ").last ?? "").lowercased()
                if c != "." || !(abbreviations.contains(before) || before.allSatisfy(\.isNumber)) { return n }
            }
            i = n
        }
        return s.count > 240 ? s.lastIndex(of: " ").map { s.index(after: $0) } : nil   // (a very long run: at a space)
    }

    // What the voice should actually say: no markdown, no links
    static func speakable(_ s: String) -> String {
        var t = s.replacingOccurrences(of: #"https?://\S+"#, with: "a link", options: .regularExpression)
        t = t.replacingOccurrences(of: #"[*_#`>|]"#, with: "", options: .regularExpression)
        t = t.replacingOccurrences(of: #"^\s*[-•]\s+"#, with: "", options: .regularExpression)
        return t.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    static func words(_ s: String) -> Set<String> {
        Set(s.lowercased().split { !$0.isLetter && !$0.isNumber && $0 != "'" }.map(String.init).filter { $0.count > 1 })
    }
}
