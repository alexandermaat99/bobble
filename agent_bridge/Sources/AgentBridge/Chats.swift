// What each Claude Code chat is doing, worked out from its hook events (and its transcript, for what Claude has said,
// the model and the chat's title), and what the display should show for it.
import Foundation

enum ChatState: String { case idle, working, waiting, done, stopped }   // (stopped: you interrupted it)

final class Chat {
    let id: String
    var agent = "claude"            // the app it's in ("claude" = Claude Code, "codex", "cursor", "conductor"): its mark shows
    var runtime = "claude"          // the agent actually running it ("claude", "codex", "cursor"): how to read its log
    var said = ""                   // (Cursor) what the agent last said, for the summary when it stops
    var project = "", model = "", title = "", tool = "", prompt = "", cwd = ""
    var home = ""                   // the folder it started in (its project), from its log; cwd moves about with cd
    var state = ChatState.idle
    var detail = ""                 // what the display shows
    var ask = ""                    // what the last tool was about to do, as a question (for a permission prompt after)
    var working = ""                // the last working message, put back after a tool (or a permission prompt) finishes
    var turnDone = false            // finished, and no new prompt since: tool calls now are housekeeping, not work
    var changed = Date()
    var heard = Date()              // its last hook event
    var pinged = Date()             // the last time we reminded the display it's still open
    var inTool = false              // a tool is running (between Pre- and PostToolUse): long silences are fine then
    var transcript = ""             // its log (Claude Code's or Codex's), for spotting an interrupt
    var logSize: UInt64 = 0         // its size when last checked (unchanged: nothing new to look at)
    // Waiting on a choice you can tap on the display ("Yes" / "No", a question's answers): Claude Code's hook is held
    // open meanwhile. answer: what a tap on choice i does; release: lets the hook go undecided (the terminal asks)
    var options: [String] = []
    var qid = ""
    var answer: ((Int) -> Void)?
    var release: (() -> Void)?
    init(id: String) { self.id = id }
}

final class Chats {
    private(set) var all: [String: Chat] = [:]
    var hideText = false            // privacy: the display just says "Working", "Needs you", "Done"
    var onUpdate: (Chat) -> Void = { _ in }

    // Claude Code's hooks, and Codex's (the same shape: OpenAI modelled them on Claude Code's; agent "codex")
    func handle(_ j: [String: Any], agent: String = "claude") {
        let event = j["hook_event_name"] as? String ?? ""
        guard let raw = j["session_id"] as? String, !raw.isEmpty else { return }
        let sid = agent == "claude" ? raw : "\(agent)-\(raw)"
        let codex = agent == "codex"
        let chat = all[sid] ?? Chat(id: sid)
        all[sid] = chat
        chat.runtime = agent
        chat.agent = sourceApp(j["cwd"] as? String ?? "", agent)
        let tool = codex ? codexTool(j["tool_name"] as? String ?? "") : j["tool_name"] as? String ?? ""
        let input = j["tool_input"] as? [String: Any] ?? [:]
        let transcript = j["transcript_path"] as? String ?? ""
        if let cwd = j["cwd"] as? String { chat.cwd = cwd }
        if !transcript.isEmpty { chat.transcript = transcript }
        if chat.home.isEmpty, !chat.transcript.isEmpty { chat.home = Transcript.startDir(chat.transcript) ?? "" }
        let dir = chat.home.isEmpty ? chat.cwd : chat.home      // named for where it started, not where it's cd'd to
        if !dir.isEmpty {
            chat.project = dir == FileManager.default.homeDirectoryForCurrentUser.path ? "Home" : (dir as NSString).lastPathComponent
        }
        chat.heard = Date()
        if ["PostToolUse", "UserPromptSubmit", "Stop", "SessionEnd"].contains(event) { letGo(chat) }   // (answered in the terminal)
        if event == "PreToolUse" { chat.inTool = true }
        if event == "PostToolUse" || event == "Stop" || event == "UserPromptSubmit" { chat.inTool = false }

        switch event {                               // the turn: tool calls after it's finished don't count
        case "UserPromptSubmit": chat.turnDone = false; chat.working = ""
        case "Stop": chat.turnDone = true
        case "PreToolUse", "PostToolUse": if chat.turnDone { return }
        default: break
        }
        if codex {                                   // (its transcript is in its own format: model and title from here)
            if let m = j["model"] as? String, !m.isEmpty { chat.model = prettyCursorModel(m) }
            if chat.title.isEmpty, event == "UserPromptSubmit" { chat.title = String(summarize(j["prompt"] as? String ?? "").prefix(40)) }
        } else {
            if let m = j["model"] as? String, !m.isEmpty { chat.model = prettyModel(m) }
            else if event == "UserPromptSubmit" || event == "Stop" || chat.model.isEmpty,
                    let m = Transcript.lastModel(transcript) { chat.model = prettyModel(m) }
            if event == "UserPromptSubmit" || event == "Stop" || chat.title.isEmpty,
               let t = Transcript.title(transcript) { chat.title = t }
        }

        var state = ChatState.working, detail = ""
        switch event {
        case "UserPromptSubmit":
            detail = "Thinking"
            chat.prompt = summarize(j["prompt"] as? String ?? "")
        case "PostToolUse":
            detail = chat.working.isEmpty ? "Thinking" : chat.working
        case "PreToolUse":
            chat.tool = integration(tool)
            var ask = ""
            let file = ((input["file_path"] ?? input["notebook_path"]) as? String).map { ($0 as NSString).lastPathComponent } ?? ""
            switch tool {
            case "Edit", "Write", "MultiEdit", "NotebookEdit":
                detail = file.isEmpty ? "Editing files" : "Editing \(file)"; ask = file.isEmpty ? "Apply these edits?" : "Edit \(file)?"
            case "Read": detail = "Reading \(file)"; ask = "Read \(file)?"
            case "Bash":
                let cmd = input["command"] as? String ?? ""
                detail = (input["description"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? "Running \(commandName(cmd))"
                ask = "Run \(String((cmd.split(separator: "\n").first ?? "").prefix(100)))?"
            case "Grep", "Glob": detail = "Searching"
            case "WebFetch": detail = "Browsing the web"; ask = "Open \(String((input["url"] as? String ?? "").prefix(90)))?"
            case "WebSearch": detail = "Browsing the web"
            case "Task", "Agent": detail = "Delegating"
            case "TodoWrite": detail = "Planning"
            case "AskUserQuestion":
                state = .waiting
                detail = ((input["questions"] as? [[String: Any]])?.first?["question"] as? String) ?? "Has a question"
            default:
                detail = tool.hasPrefix("mcp__") ? "Using \(mcpServer(tool).replacingOccurrences(of: "_", with: " "))" : "Using \(tool)"
            }
            chat.ask = ask.isEmpty ? "\(detail.isEmpty ? "Use \(tool)" : detail)?" : ask
            if state == .working {                   // what Claude says it's doing beats the bare action
                let words = codex ? Transcript.codexNarration(transcript) : Transcript.narration(transcript)
                if let said = words.map(summarize), !said.isEmpty { detail = said }
                chat.working = detail
            }
        case "PermissionRequest":                    // it's asking you to approve something
            state = .waiting
            detail = codex ? (input["description"] as? String).flatMap { $0.isEmpty ? nil : "\($0)?" } ?? chat.ask : chat.ask
            if detail.isEmpty { detail = "Needs your permission" }
        case "Notification":
            // permission prompts and questions need you; the "idle for 60 s" reminder doesn't (it already shows done)
            let message = j["message"] as? String ?? ""
            switch j["notification_type"] as? String ?? "" {
            case "permission_prompt": detail = chat.ask
            case "elicitation_dialog": detail = message
            case "idle_prompt", "auth_success": return
            default:
                guard message.contains("ermission") else { return }
                detail = chat.ask
            }
            state = .waiting
            if detail.isEmpty { detail = "Needs your permission" }
        case "Stop":
            // what it did: the start of Claude's final reply (Claude Code hands it over; the transcript may lag), else
            // what you'd asked for
            // ...unless it ended by asking you something ("Want me to build that?"): then it's waiting on you, and the
            // question is what shows
            let reply = j["last_assistant_message"] as? String ?? ""
            if let q = closingQuestion(reply) {
                state = .waiting
                detail = q
            } else {
                state = .done
                detail = summarize(reply)
                if ["", "Done", "Done.", "done"].contains(detail) { detail = chat.prompt.isEmpty ? "Finished" : "Finished: \(chat.prompt)" }
            }
            chat.tool = ""
        case "SessionEnd":
            state = .idle
            chat.tool = ""
        default:
            return
        }
        if event == "UserPromptSubmit" { chat.tool = "" }
        chat.state = state
        chat.detail = detail
        chat.changed = Date()
        onUpdate(chat)
        if state == .idle { all[sid] = nil }
    }

    // Every couple of seconds: a working (or waiting) chat you've interrupted (Ctrl+C, Esc) turns "stopped": its last
    // message stays up, greyed, until you close the chat or send it something new (the agents don't send a hook for an
    // interrupt, but they write it into their log). One that's gone quiet for 5 minutes with no tool running, and no
    // new lines in its log either, is let go.
    func check() {
        let now = Date()
        for c in all.values {
            // still open: remind the display every few minutes (it forgets a chat it hasn't heard of in 15), until it's
            // been 2 hours with nothing happening, when it's most likely gone without saying (a force-quit terminal)
            if c.state != .idle, now.timeIntervalSince(c.changed) > 7200 { set(c, .idle, ""); all[c.id] = nil; continue }
            if c.state != .idle, now.timeIntervalSince(max(c.heard, c.pinged)) > 240 { c.pinged = now; onUpdate(c) }
            guard c.state == .working || c.state == .waiting else { continue }
            let size = (try? FileManager.default.attributesOfItem(atPath: c.transcript)[.size] as? NSNumber)?.uint64Value ?? 0
            let grew = size != c.logSize
            c.logSize = size
            if grew, Transcript.interrupted(c.transcript, codex: c.runtime == "codex") {   // (only when its log changed)
                set(c, .stopped, c.detail.isEmpty ? "Stopped" : c.detail)
                c.turnDone = true                        // (its stray tool events don't count now)
                c.inTool = false
                continue
            }
            let logAge = (try? FileManager.default.attributesOfItem(atPath: c.transcript)[.modificationDate] as? Date)
                .map { now.timeIntervalSince($0) } ?? .infinity
            if !c.inTool, now.timeIntervalSince(c.heard) > 300, logAge > 300 { set(c, .idle, ""); all[c.id] = nil }
        }
    }

    // Claude Code's hooks that wait for an answer from the display (HookServer's /permission): PermissionRequest for
    // a tool ("Yes" / "No"), PreToolUse for AskUserQuestion (its answers: up to four, one to pick, and several
    // questions one after another). Nothing tapped within 85 s, or anything else happening in the chat: the hook goes
    // with no decision, and Claude Code asks in the terminal as usual.
    func handlePermission(_ j: [String: Any], reply: @escaping HookServer.Reply) {
        guard let sid = j["session_id"] as? String, !sid.isEmpty else { reply(""); return }
        let event = j["hook_event_name"] as? String ?? "", tool = j["tool_name"] as? String ?? ""
        let input = j["tool_input"] as? [String: Any] ?? [:]
        let asking = tool == "AskUserQuestion"
        if asking != (event == "PreToolUse") { reply(""); return }   // (a question's answered at PreToolUse, not here)
        // (Conductor shows a question only once the hooks before it are done: holding one there would hide it)
        if asking, (j["cwd"] as? String ?? "").contains("/conductor/workspaces/") { reply(""); handle(j); return }
        if let old = all[sid] { letGo(old) }
        // the choices, one set per step: a question's answers (several questions: one after another), or Yes / No
        var steps: [(text: String, options: [String])] = []
        var finish: ([String]) -> [String: Any]
        if asking {
            let qs = input["questions"] as? [[String: Any]] ?? []
            for q in qs {
                let text = q["question"] as? String ?? ""
                let opts = ((q["options"] as? [[String: Any]]) ?? []).compactMap { $0["label"] as? String }
                guard q["multiSelect"] as? Bool != true, !text.isEmpty, (1...4).contains(opts.count) else { steps = []; break }
                steps.append((text, opts))
            }
            guard !steps.isEmpty, steps.count == qs.count else { reply(""); handle(j); return }
            finish = { picked in
                var answered = input
                var answers: [String: String] = [:]
                for (k, st) in steps.enumerated() { answers[st.text] = picked[k] }
                answered["answers"] = answers
                return ["hookEventName": "PreToolUse", "permissionDecision": "allow", "updatedInput": answered]
            }
        } else {
            steps = [("", ["Yes", "No"])]
            finish = { picked in
                ["hookEventName": "PermissionRequest",
                 "decision": picked[0] == "Yes" ? ["behavior": "allow"] : ["behavior": "deny", "message": "The user said no (on their Bobble display)."]]
            }
        }
        let chat = all[sid] ?? Chat(id: sid)
        all[sid] = chat
        var picked: [String] = []
        chat.release = { reply("") }
        // shows step k's choices (each step its own id, so the display takes them as new) and lets go of the hook if
        // nobody taps within 85 s (curl gives up at 90): the terminal asks then
        func show(_ k: Int) {
            let qid = String(UUID().uuidString.prefix(8))
            chat.options = steps[k].options
            chat.qid = qid
            DispatchQueue.main.asyncAfter(deadline: .now() + 85) { [weak self, weak chat] in
                guard let self, let chat, chat.qid == qid else { return }
                self.letGo(chat)
                self.onUpdate(chat)                  // (the choices go from the display: it's the terminal's now)
            }
        }
        let pick = { [weak self, weak chat] (label: String) in
            guard let self, let chat, chat.release != nil else { return }
            picked.append(label)
            if picked.count < steps.count {          // the next question
                show(picked.count)
                self.set(chat, .waiting, steps[picked.count].text)
                return
            }
            let out = (try? JSONSerialization.data(withJSONObject: ["hookSpecificOutput": finish(picked)])).map { String(decoding: $0, as: UTF8.self) } ?? ""
            chat.release = nil
            reply(out)
            self.letGo(chat)
            self.set(chat, .working, chat.working.isEmpty ? "Thinking" : chat.working)
        }
        chat.answer = { [weak chat] i in
            guard let chat, i < chat.options.count else { return }
            pick(chat.options[i])
        }
        show(0)
        handle(j)                                    // (waiting, with the (first) question)
    }

    // A choice tapped on the display (Device.fetchAnswers)
    func answer(session: String, qid: String, choice: Int) {
        guard let c = all[session], c.qid == qid, !qid.isEmpty else { return }
        c.answer?(choice)
    }

    // Its held hook, if any, let go undecided; no choices on the display
    private func letGo(_ c: Chat) {
        c.release?()
        c.release = nil
        c.answer = nil
        c.options = []
        c.qid = ""
    }

    private func set(_ c: Chat, _ state: ChatState, _ detail: String) {
        if state != .waiting { letGo(c) }
        c.state = state
        c.detail = detail
        c.changed = Date()
        onUpdate(c)
    }

    // Cursor's hooks (see Hooks.cursorEvents). Only ones that can't block anything: Cursor reads a "before" hook's
    // silence as "deny", and answering "allow" would skip its own approval prompts. So no "needs you" from Cursor yet.
    func handleCursor(_ j: [String: Any]) {
        let event = j["hook_event_name"] as? String ?? ""
        guard let cid = j["conversation_id"] as? String, !cid.isEmpty else { return }
        let id = "cursor-" + cid
        let chat = all[id] ?? Chat(id: id)
        all[id] = chat
        chat.agent = "cursor"
        chat.runtime = "cursor"
        if let root = (j["workspace_roots"] as? [String])?.first { chat.project = (root as NSString).lastPathComponent }
        if let m = j["model"] as? String, !m.isEmpty { chat.model = prettyCursorModel(m) }
        let file = (j["file_path"] as? String).map { ($0 as NSString).lastPathComponent } ?? ""

        var state = ChatState.working, detail = ""
        switch event {
        case "beforeSubmitPrompt":
            let p = summarize(j["prompt"] as? String ?? "")
            chat.prompt = p
            if chat.title.isEmpty { chat.title = String(p.prefix(40)) }   // (Cursor doesn't name its chats for us)
            chat.said = ""
            chat.tool = ""
            detail = "Thinking"
        case "afterAgentResponse":                   // what it says as it goes: the best line to show
            chat.said = j["text"] as? String ?? ""
            detail = summarize(chat.said)
            if detail.isEmpty { return }
        case "afterFileEdit": detail = "Editing \(file)"
        case "afterShellExecution": detail = "Running \(commandName(j["command"] as? String ?? ""))"
        case "afterMCPExecution":
            let server = j["mcp_server_name"] as? String ?? ""
            chat.tool = integration("mcp__\(server)__x")
            detail = "Using \(server.replacingOccurrences(of: "_", with: " "))"
        case "postToolUse":
            let tool = j["tool_name"] as? String ?? ""
            if tool.hasPrefix("MCP") || tool == "Shell" || tool.contains("Edit") || tool == "Write" { return }   // (said above)
            detail = tool == "Read" || tool == "ReadFile" ? "Reading" : tool.isEmpty ? "Working" : "Using \(tool)"
        case "stop":
            switch j["status"] as? String ?? "completed" {
            case "aborted": state = .stopped; detail = chat.detail.isEmpty ? "Stopped" : chat.detail
            case "error": state = .done; detail = "Hit an error"
            default:
                state = .done
                detail = summarize(chat.said)
                if detail.isEmpty { detail = chat.prompt.isEmpty ? "Finished" : "Finished: \(chat.prompt)" }
            }
            chat.tool = ""
        case "sessionEnd":
            state = .idle
        default:
            return
        }
        chat.state = state
        chat.detail = detail
        chat.changed = Date()
        onUpdate(chat)
        if state == .idle { all[id] = nil }
    }

    // The message for the display: plain ASCII (its font), under 200 characters, ending on a whole word
    func payload(_ c: Chat) -> [String: Any] {
        var d = hideText ? ["working": "Working", "waiting": "Needs you", "done": "Done", "stopped": "Stopped", "idle": ""][c.state.rawValue]! : c.detail
        d = String(d.unicodeScalars.filter { $0.value >= 32 && $0.value < 127 }.map(Character.init))
        if d.count > 190 { d = String(d.prefix(190)); if let sp = d.lastIndex(of: " ") { d = String(d[..<sp]) }; d += "..." }
        let ascii = { (s: String) in String(s.unicodeScalars.filter { $0.value >= 32 && $0.value < 127 }.map(Character.init)) }
        var p: [String: Any] = ["session": c.id, "state": c.state.rawValue, "detail": d, "project": ascii(c.project),
                "model": ascii(c.model), "title": hideText ? "" : ascii(c.title), "tool": c.tool, "agent": c.agent,
                "provider": provider(c.model, c.runtime),
                "t": Int64(Date().timeIntervalSince1970 * 1000)]
        if c.state == .waiting, !c.options.isEmpty {   // (choices to tap: each cut to the display's 31 characters)
            p["options"] = c.options.map { String(ascii($0).prefix(31)) }
            p["qid"] = c.qid
        }
        return p
    }
}

// ---- turning Claude Code's names into the display's

// claude-opus-5-5 -> "Opus 5.5", claude-3-5-sonnet-20241022 -> "Sonnet 3.5" ([1m]: the long-context variant, same model)
func prettyModel(_ id: String) -> String {
    var s = id.hasPrefix("claude-") ? String(id.dropFirst(7)) : id
    if let b = s.firstIndex(of: "[") { s = String(s[..<b]) }
    var fam = "", ver: [String] = []
    for p in s.split(separator: "-") {
        if p.allSatisfy(\.isNumber) { if p.count <= 2 { ver.append(String(p)) } }
        else if fam.isEmpty { fam = p.prefix(1).uppercased() + p.dropFirst() }
    }
    return ver.isEmpty ? fam : "\(fam) \(ver.joined(separator: "."))"
}

// Cursor's (and Codex's) model slugs as the display shows them: "claude-4.5-sonnet" -> "Sonnet 4.5", "gpt-5" -> "GPT-5",
// "composer-1" -> "Composer 1"
func prettyCursorModel(_ m: String) -> String {
    if m.hasPrefix("claude") {
        let parts = m.split(separator: "-")
        let fam = parts.first { $0.first?.isLetter == true && $0 != "claude" } ?? ""
        let ver = parts.first { $0.first?.isNumber == true } ?? ""
        return [fam.prefix(1).uppercased() + fam.dropFirst(), String(ver)].filter { !$0.isEmpty }.joined(separator: " ")
    }
    if m.hasPrefix("gpt") || m.hasPrefix("o") && m.dropFirst().first?.isNumber == true {   // "gpt-5.5-codex" -> "GPT-5.5 Codex"
        let parts = m.split(separator: "-")
        let head = parts.prefix(2).joined(separator: "-").uppercased(), rest = parts.dropFirst(2).map { $0.prefix(1).uppercased() + $0.dropFirst() }
        return ([head] + rest).joined(separator: " ")
    }
    return m.split(separator: "-").map { $0.prefix(1).uppercased() + $0.dropFirst() }.joined(separator: " ")
}

// Who made the model doing the work, for the mark at the top: from its name, else the agent's usual one
func provider(_ model: String, _ runtime: String) -> String {
    let m = model.lowercased()
    if ["claude", "opus", "sonnet", "haiku", "fable"].contains(where: m.contains) { return "claude" }
    if m.hasPrefix("gpt") || m.contains("codex") || (m.first == "o" && m.dropFirst().first?.isNumber == true) { return "openai" }
    if m.contains("gemini") { return "googlegemini" }
    if m.contains("composer") || m.contains("cursor") { return "cursor" }
    return ["claude": "claude", "codex": "openai", "cursor": "cursor"][runtime] ?? ""
}

// The app a chat is in, for its logo: Conductor runs every chat (Claude Code or Codex) in a workspace folder of its
// own (~/conductor/workspaces/<repo>/<name>); otherwise it's the agent itself (Claude Code, Codex in a terminal)
func sourceApp(_ cwd: String, _ agent: String) -> String {
    cwd.contains("/conductor/workspaces/") ? "conductor" : agent
}

// Codex's tool names in Claude Code's terms, so they read the same on the display
func codexTool(_ t: String) -> String {
    switch t {
    case "shell", "local_shell", "exec_command", "unified_exec", "container.exec": return "Bash"
    case "apply_patch": return "Edit"
    case "update_plan": return "TodoWrite"
    case "web_search", "web_search_preview": return "WebSearch"
    case "view_image": return "Read"
    default: return t
    }
}

// mcp__claude_ai_Google_Calendar__list -> "Google_Calendar"
func mcpServer(_ tool: String) -> String {
    var s = String(tool.dropFirst(5))
    if let r = s.range(of: "__") { s = String(s[..<r.lowerBound]) }
    if s.hasPrefix("claude_ai_") { s = String(s.dropFirst(10)) }
    return s
}

// The integration a tool belongs to, as the display knows its logos: mcp__claude_ai_Google_Calendar__list -> googlecalendar
func integration(_ tool: String) -> String {
    guard tool.hasPrefix("mcp__") else { return "" }
    let k = mcpServer(tool).lowercased().filter { $0.isLetter || $0.isNumber }
    switch k {
    case "googlecloudbigquery": return "googlebigquery"
    case "googledocs", "googlesheets", "googleslides": return "googledrive"
    default: return k
    }
}

// The program a shell command runs, for one without a description: skips "cd dir &&", VAR=value, sudo/env/time and
// options ("cd app && FOO=1 npm test" -> npm); a loop, function or other shell construct is just "a script"
func commandName(_ cmd: String) -> String {
    let w = cmd.split(whereSeparator: { $0 == " " || $0 == "\n" || $0 == "\t" }).map(String.init)
    var i = 0
    while i < w.count {
        let x = w[i]
        if x == "cd" || x == "pushd" { i += 2; if i < w.count, ["&&", ";", "||"].contains(w[i]) { i += 1 }; continue }
        if [";", "&&", "||", "sudo", "time", "env", "nohup", "command", "exec"].contains(x) { i += 1; continue }
        if x.hasPrefix("-") || (x.contains("=") && x.first.map { $0.isLetter || $0 == "_" } == true) { i += 1; continue }
        break
    }
    guard i < w.count else { return w.first ?? "" }
    var n = w[i].trimmingCharacters(in: CharacterSet(charactersIn: "\"'"))
    if n.hasSuffix(";") { n.removeLast() }
    if ["for", "while", "until", "if", "case", "select", "function", "{", "(", "((", "[["].contains(n) || n.hasSuffix("()") ||
       (i + 1 < w.count && (w[i + 1] == "()" || w[i + 1] == "(")) { return "a script" }
    return (n as NSString).lastPathComponent
}

// Some of the agent's text as one plain line for the bubble: its first sentence or two of prose, up to ~140 characters.
// Code blocks, tables, headings and indented code don't count. A short intro ("Two things:", "Here's what changed:")
// points at what follows, so the next line's used instead; a real sentence that just ends in a colon is kept.
func summarize(_ text: String) -> String {
    var prose: [String] = [], fenced = false
    for raw in text.split(separator: "\n", omittingEmptySubsequences: false).map(String.init) {
        let l = raw.trimmingCharacters(in: .whitespaces)
        if l.hasPrefix("```") || l.hasPrefix("~~~") { fenced.toggle(); continue }
        if fenced || l.isEmpty || l.hasPrefix("|") || l.hasPrefix("#") || raw.hasPrefix("    ") || raw.hasPrefix("\t") { continue }
        prose.append(l)
    }
    guard !prose.isEmpty else { return "" }
    let intro = prose.count > 1 && prose[0].hasSuffix(":") && plain(prose[0]).count <= 40
    var t = plain(prose[intro ? 1 : 0])
    if t.hasSuffix(":") { t.removeLast() }
    if t.count > 140 {
        let cut = String(t.prefix(140))
        if let r = cut.range(of: #"^.*[.!?](?= )"#, options: .regularExpression) { t = String(cut[r]) }   // whole sentences
        else { t = (cut.lastIndex(of: " ").map { String(cut[..<$0]) } ?? cut) + "..." }
    }
    return t
}

// The question a reply ends on, if its last paragraph asks one: that sentence ("Want me to build that?"), even with a
// remark after it ("...? I can also do just the calendar first.")
func closingQuestion(_ text: String) -> String? {
    var last = "", fenced = false
    for raw in text.split(separator: "\n").map(String.init) {
        let l = raw.trimmingCharacters(in: .whitespaces)
        if l.hasPrefix("```") || l.hasPrefix("~~~") { fenced.toggle(); continue }
        if fenced || l.hasPrefix("|") || l.hasPrefix("#") || raw.hasPrefix("    ") || raw.hasPrefix("\t") { continue }
        last = l
    }
    let t = plain(last)
    guard let r = t.range(of: #"[^.!?]*\?(?=\s|$)"#, options: [.regularExpression, .backwards]) else { return nil }
    var q = t[r].trimmingCharacters(in: .whitespaces)
    if q.count > 180 { q = "..." + q.suffix(177) }
    return q.count >= 4 ? q : nil
}

// A line of markdown as plain text: links, emphasis, code marks, quote marks, bullets and list numbers gone
private func plain(_ line: String) -> String {
    var t = line.replacingOccurrences(of: #"\[([^\]]*)\]\([^)]*\)"#, with: "$1", options: .regularExpression)
    t = t.replacingOccurrences(of: #"[*`_]"#, with: "", options: .regularExpression)
    t = t.replacingOccurrences(of: #"^[\s>•-]+"#, with: "", options: .regularExpression)
    t = t.replacingOccurrences(of: #"^[0-9]+[.)] +"#, with: "", options: .regularExpression)
    return t.replacingOccurrences(of: #"\s+"#, with: " ", options: .regularExpression).trimmingCharacters(in: .whitespaces)
}

// ---- reading the chat's transcript (Claude Code's JSONL log of it)

enum Transcript {
    // The last ~n lines (from the last 512 KB of the file, so a long chat stays cheap)
    static func tail(_ path: String, _ n: Int, bytes: UInt64 = 512_000) -> [[String: Any]] {
        guard let f = FileHandle(forReadingAtPath: path) else { return [] }
        defer { try? f.close() }
        let size = (try? f.seekToEnd()) ?? 0
        try? f.seek(toOffset: size > bytes ? size - bytes : 0)
        let data = (try? f.readToEnd()) ?? Data()
        return data.split(separator: UInt8(ascii: "\n")).suffix(n)
            .compactMap { (try? JSONSerialization.jsonObject(with: Data($0))) as? [String: Any] }
    }

    // The folder the chat started in: the first "cwd" in its log
    static func startDir(_ path: String) -> String? {
        guard let f = FileHandle(forReadingAtPath: path) else { return nil }
        defer { try? f.close() }
        let head = (try? f.read(upToCount: 400_000)) ?? Data()
        for line in head.split(separator: UInt8(ascii: "\n")) {
            if let j = (try? JSONSerialization.jsonObject(with: Data(line))) as? [String: Any], let d = j["cwd"] as? String, !d.isEmpty { return d }
        }
        return nil
    }

    static func lastModel(_ path: String) -> String? {
        tail(path, 60).last { $0["type"] as? String == "assistant" && ($0["message"] as? [String: Any])?["model"] != nil }
            .flatMap { ($0["message"] as? [String: Any])?["model"] as? String }
    }

    // The chat's title, as Claude Code names it ("Access to other chats"): the newest one in the file
    // (read backwards from the end, 1 MB at a time, up to 16 MB: a long chat's log runs to tens of MB, and the
    // newest title is usually near the end)
    static func title(_ path: String) -> String? {
        guard let f = FileHandle(forReadingAtPath: path) else { return nil }
        defer { try? f.close() }
        let size = (try? f.seekToEnd()) ?? 0, chunk: UInt64 = 1_000_000, marker = Data(#""type":"ai-title""#.utf8)
        var end = size
        while end > 0, size - end < 16_000_000 {
            let start = end > chunk ? end - chunk : 0
            try? f.seek(toOffset: start)
            guard let data = try? f.read(upToCount: Int(min(end - start + 4096, size - start))) else { return nil }   // (+ a bit: a line across the edge)
            if let r = data.range(of: marker, options: .backwards),
               let ls = data[..<r.lowerBound].lastIndex(of: UInt8(ascii: "\n")).map({ $0 + 1 }) ?? (start == 0 ? data.startIndex : nil),
               let le = data[r.upperBound...].firstIndex(of: UInt8(ascii: "\n")) {
                return ((try? JSONSerialization.jsonObject(with: data[ls..<le])) as? [String: Any])?["aiTitle"] as? String
            }
            end = start
        }
        return nil
    }

    // Did you interrupt it? Claude Code writes "[Request interrupted by user…]" as the newest message; Codex logs a
    // turn_aborted event
    static func interrupted(_ path: String, codex: Bool) -> Bool {
        guard !path.isEmpty else { return false }
        let lines = tail(path, 20, bytes: 32_000)
        if codex {
            let last = lines.last { $0["type"] as? String == "event_msg" }.flatMap { $0["payload"] as? [String: Any] }
            return last?["type"] as? String == "turn_aborted"
        }
        guard let last = lines.last(where: { ["user", "assistant"].contains($0["type"] as? String) }),
              last["type"] as? String == "user", let m = last["message"] as? [String: Any] else { return false }
        let texts = (m["content"] as? String).map { [$0] } ??
            ((m["content"] as? [[String: Any]]) ?? []).compactMap { $0["text"] as? String }
        return texts.contains { $0.hasPrefix("[Request interrupted by user") }
    }

    // (Codex) what it last said in this turn: its rollout log's agent_message events since your last message, or nil
    static func codexNarration(_ path: String) -> String? {
        let lines = tail(path, 120).filter { $0["type"] as? String == "event_msg" }
        let payload = { (e: [String: Any]) in e["payload"] as? [String: Any] ?? [:] }
        let since = (lines.lastIndex { payload($0)["type"] as? String == "user_message" } ?? -1) + 1
        return lines[since...].last { payload($0)["type"] as? String == "agent_message" }.flatMap { payload($0)["message"] as? String }
    }

    // What Claude last said in this turn, or nil if it hasn't since your prompt, or has run more than 2 tools since
    // (it's moved on; and the transcript doesn't always have its latest words yet)
    static func narration(_ path: String) -> String? {
        let lines = tail(path, 80)
        let isPrompt = { (e: [String: Any]) -> Bool in
            guard e["type"] as? String == "user", let m = e["message"] as? [String: Any] else { return false }
            if m["content"] is String { return true }
            return (m["content"] as? [[String: Any]])?.contains { $0["type"] as? String == "text" } ?? false
        }
        let since = (lines.lastIndex(where: isPrompt) ?? -1) + 1
        let items = lines[since...].filter { $0["type"] as? String == "assistant" }
            .flatMap { (($0["message"] as? [String: Any])?["content"] as? [[String: Any]]) ?? [] }
            .filter { ["text", "tool_use"].contains($0["type"] as? String) }
        guard let k = items.lastIndex(where: { $0["type"] as? String == "text" }), items.count - k - 1 <= 2 else { return nil }
        return items[k]["text"] as? String
    }
}
