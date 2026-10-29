// What Claude can do in a voice chat: look at your coding agents and answer the ones waiting on you, start a Claude
// Code task in a project folder (in Terminal, so you can follow it; it shows on the Bobble like any chat), read the
// calendar and work Spotify (both through the board, which has them), search the web (the API's own tool), and work
// in your connected apps - Google Calendar, Gmail, Drive, Linear, Slack, Notion - through Claude Code on this Mac.
import AppKit

enum VoiceTools {
    struct Output { let text: String; var error = false }

    static func definitions(model: String) -> [[String: Any]] {
        func tool(_ name: String, _ description: String, _ props: [String: Any] = [:], _ required: [String] = []) -> [String: Any] {
            ["name": name, "description": description,
             "input_schema": ["type": "object", "properties": props, "required": required, "additionalProperties": false]]
        }
        let str = { (d: String) -> [String: Any] in ["type": "string", "description": d] }
        return [
            tool("coding_agents", "The coding agent chats open on the user's Mac (Claude Code, Codex, Cursor, Conductor): each one's id, project, title, whether it's working, waiting on the user or done, what it's doing or asking, and the answers it's offering when it's waiting on a choice. Call this whenever they ask about their agents or chats, what's running, how a task is going, or whether anything needs them."),
            tool("answer_agent", "Answer a coding agent that's waiting on the user's choice (a permission prompt's Yes or No, or a question's options), exactly as if they'd tapped it. Call this only once they've told you what to answer.",
                 ["chat": str("The chat's id from coding_agents"), "answer": str("One of the answers it offers, word for word")], ["chat", "answer"]),
            tool("project_folders", "The user's project folders (where their coding agents have been working, and git repositories in their usual places), most recent first. Call this before start_coding_task when you're not sure which folder they mean."),
            tool("start_coding_task", "Start a new Claude Code chat on the user's Mac, in a project folder, with a task to do. It opens in Terminal and shows on the Bobble while it works; its permission prompts can be answered from the Bobble or with answer_agent. Call this when they ask you to get Claude Code (or an agent) to build, fix, change or look into something in one of their projects.",
                 ["folder": str("The project folder: a full path, or a folder's name from project_folders"),
                  "task": str("What to ask Claude Code to do, written as the user's request")], ["folder", "task"]),
            tool("calendar", "The user's calendar for the rest of today and tomorrow. Call this when they ask about their schedule, meetings, what's next, or when they're free."),
            tool("music", "Control Spotify on the user's account, or see what's playing. Call this when they ask to play, pause, skip, go back, turn it up or down, or what song this is.",
                 ["action": ["type": "string", "enum": ["status", "play", "pause", "next", "previous", "louder", "quieter"]]], ["action"]),
            tool("connected_apps", "Have Claude Code on the user's Mac do something in their connected apps: Google Calendar (see, add, move or cancel events, find free time), Gmail, Google Drive, Linear (issues, projects), Slack (messages, channels) and Notion. It takes about 5 to 15 seconds, so say a few words first. For anything that changes something (adding, moving or deleting an event, sending an email or a message, posting, creating or updating an issue or page), first say exactly what you'll do and wait for their yes; then call this with every detail in the request. Prefer the calendar tool for a quick look at today and tomorrow.",
                 ["request": str("What to do or find out, in full: which app, and every detail (names, dates and times with the day, wording of anything to send)")], ["request"]),
            tool("end_chat", "End the voice chat, once you've said goodbye (the user said bye, or they're done)."),
            // (the newer web search for models that have it; Haiku has the basic one)
            ["type": model.contains("haiku") ? "web_search_20250305" : "web_search_20260209", "name": "web_search", "max_uses": 3],
        ]
    }

    @MainActor static func run(_ name: String, _ input: [String: Any], chat: VoiceChat) async -> Output {
        // (what it's doing, under the conversation: the voice may say nothing while a tool runs)
        let doing = ["coding_agents": "Looking at your agents", "answer_agent": "Answering your agent",
                     "project_folders": "Looking at your projects", "start_coding_task": "Starting Claude Code",
                     "calendar": "Checking your calendar", "music": "Talking to Spotify",
                     "connected_apps": "Asking Claude Code"][name]
        if let doing { chat.doing(doing) }
        defer { if doing != nil { chat.doing("") } }
        switch name {
        case "coding_agents": return Output(text: agents(chat.agentChats))
        case "answer_agent": return answer(chat.agentChats, input["chat"] as? String ?? "", input["answer"] as? String ?? "")
        case "project_folders":
            let f = folders(chat.agentChats)
            return Output(text: f.isEmpty ? "No project folders found." : f.map { ($0 as NSString).abbreviatingWithTildeInPath }.joined(separator: "\n"))
        case "start_coding_task":
            return start(chat.agentChats, input["folder"] as? String ?? "", input["task"] as? String ?? "")
        case "calendar":
            return Output(text: await chat.askBoard("agenda") ?? "The Bobble didn't answer.")
        case "music":
            let a = input["action"] as? String ?? "status"
            return Output(text: await chat.askBoard("music", a) ?? "The Bobble didn't answer.")
        case "connected_apps":
            return await apps(input["request"] as? String ?? "")
        case "end_chat":
            chat.goodbye()
            return Output(text: "The chat will end once your goodbye has been said.")
        default:
            return Output(text: "No such tool: \(name)", error: true)
        }
    }

    // ---- coding agents

    private static func appName(_ c: Chat) -> String {
        ["claude": "Claude Code", "codex": "Codex", "cursor": "Cursor", "conductor": "Conductor"][c.agent] ?? c.agent
    }

    private static func agents(_ chats: Chats) -> String {
        let open = chats.all.values.filter { $0.state != .idle }.sorted { $0.changed > $1.changed }
        guard !open.isEmpty else { return "No coding agent chats are open right now." }
        return open.map { c in
            var parts = ["id: \(c.id)", "app: \(appName(c))"]
            if !c.project.isEmpty { parts.append("project: \(c.project)") }
            if !c.title.isEmpty { parts.append("title: \(c.title)") }
            if !c.model.isEmpty { parts.append("model: \(c.model)") }
            parts.append("state: \(c.state == .waiting ? "waiting on the user" : c.state.rawValue)")
            if !c.detail.isEmpty { parts.append("\(c.state == .waiting ? "asking" : "latest"): \(c.detail)") }
            if !c.options.isEmpty { parts.append("answers: " + c.options.map { "\"\($0)\"" }.joined(separator: ", ")) }
            let mins = Int(Date().timeIntervalSince(c.changed) / 60)
            parts.append(mins < 1 ? "changed just now" : "changed \(mins) min ago")
            return "- " + parts.joined(separator: "; ")
        }.joined(separator: "\n")
    }

    private static func answer(_ chats: Chats, _ id: String, _ want: String) -> Output {
        let key = id.lowercased()
        guard let c = chats.all[id] ?? chats.all.values.first(where: {
            $0.id.lowercased().hasPrefix(key) || (!key.isEmpty && ($0.title.lowercased() == key || $0.project.lowercased() == key))
        }) else { return Output(text: "There's no chat with id \(id). Check coding_agents.", error: true) }
        guard !c.options.isEmpty, c.answer != nil else {
            return Output(text: "That chat isn't waiting on a choice any more (it may have been answered in the terminal).", error: true)
        }
        let w = want.lowercased().trimmingCharacters(in: .whitespacesAndNewlines)
        let i = c.options.firstIndex { $0.lowercased() == w }
            ?? c.options.firstIndex { $0.lowercased().contains(w) || w.contains($0.lowercased()) }
            ?? Int(w).map { $0 - 1 }.flatMap { c.options.indices.contains($0) ? $0 : nil }
        guard let i else { return Output(text: "It offers: \(c.options.joined(separator: ", ")). Which one?", error: true) }
        let picked = c.options[i]
        c.answer?(i)
        return Output(text: "Answered \"\(picked)\" in \(c.title.isEmpty ? c.project : c.title).")
    }

    // ---- project folders and starting tasks

    // Where coding agents have worked (remembered between launches), then git repositories in the usual places
    static func folders(_ chats: Chats) -> [String] {
        var seen = UserDefaults.standard.stringArray(forKey: "chatFolders") ?? []
        let fresh = chats.all.values.sorted { $0.changed > $1.changed }.flatMap { [$0.home, $0.cwd] }.filter { !$0.isEmpty }
        for f in fresh.reversed() { seen.removeAll { $0 == f }; seen.insert(f, at: 0) }
        seen = seen.filter { !$0.contains("/conductor/workspaces/") && $0 != NSHomeDirectory() }   // (Conductor's own copies)
        seen = Array(seen.prefix(40))
        UserDefaults.standard.set(seen, forKey: "chatFolders")
        let fm = FileManager.default, home = NSHomeDirectory()
        var repos: [(String, Date)] = []
        for root in ["Developer", "Projects", "Code", "code", "src", "dev", "repos", "GitHub", "Documents/GitHub", "esp", "conductor/repos"] {
            let r = (home as NSString).appendingPathComponent(root)
            for d in (try? fm.contentsOfDirectory(atPath: r)) ?? [] where !d.hasPrefix(".") {
                let p = (r as NSString).appendingPathComponent(d)
                guard fm.fileExists(atPath: (p as NSString).appendingPathComponent(".git")) else { continue }
                let t = (try? fm.attributesOfItem(atPath: p)[.modificationDate] as? Date) ?? .distantPast
                repos.append((p, t))
            }
        }
        let more = repos.sorted { $0.1 > $1.1 }.map(\.0).filter { !seen.contains($0) }
        return (seen + more).filter { fm.fileExists(atPath: $0) }
    }

    private static func squash(_ s: String) -> String { s.lowercased().filter { $0.isLetter || $0.isNumber } }

    private static func resolve(_ chats: Chats, _ name: String) -> String? {
        let fm = FileManager.default
        let path = (name as NSString).expandingTildeInPath
        var dir: ObjCBool = false
        if path.hasPrefix("/"), fm.fileExists(atPath: path, isDirectory: &dir), dir.boolValue { return path }
        let all = folders(chats), want = squash((name as NSString).lastPathComponent)
        guard !want.isEmpty else { return nil }
        return all.first { squash(($0 as NSString).lastPathComponent) == want }
            ?? all.first { squash(($0 as NSString).lastPathComponent).contains(want) || want.contains(squash(($0 as NSString).lastPathComponent)) }
    }

    private static func start(_ chats: Chats, _ folder: String, _ task: String) -> Output {
        guard !task.trimmingCharacters(in: .whitespaces).isEmpty else { return Output(text: "No task given.", error: true) }
        guard let dir = resolve(chats, folder) else {
            let names = folders(chats).prefix(15).map { ($0 as NSString).lastPathComponent }
            return Output(text: "No folder matches \"\(folder)\". Some of theirs: \(names.joined(separator: ", ")).", error: true)
        }
        // a script Terminal runs: an interactive login shell (so `claude` is on the PATH as usual), in the folder
        let q = { (s: String) in "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }
        let script = "#!/bin/zsh -il\ncd \(q(dir)) || exit 1\nclaude \(q(task))\n"
        let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("Bobble")
        try? FileManager.default.createDirectory(at: caches, withIntermediateDirectories: true)
        let file = caches.appendingPathComponent("task-\(UUID().uuidString.prefix(8)).command")
        do {
            try script.write(to: file, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: file.path)
        } catch { return Output(text: "Couldn't write the script to start it: \(error.localizedDescription)", error: true) }
        let terminal = URL(fileURLWithPath: "/System/Applications/Utilities/Terminal.app")
        NSWorkspace.shared.open([file], withApplicationAt: terminal, configuration: NSWorkspace.OpenConfiguration())
        return Output(text: "Started Claude Code in \((dir as NSString).abbreviatingWithTildeInPath), in a new Terminal window, with the task: \(task). It shows on the Bobble as it works.")
    }

    // ---- the user's connected apps, through Claude Code
    // Claude Code on this Mac is signed in to their claude.ai account with its connectors, so a headless run (`claude
    // -p`) can use them, and anything connected there later comes along. Only those connectors' tools are allowed (no
    // files, no shell: anything else is refused, never asked about), and the Bobble's own hooks are off for the run (it
    // isn't a chat to show, and nothing should wait on a tap).
    static let connectors = ["Google_Calendar", "Gmail", "Google_Drive", "Linear", "Slack", "Notion"]

    private static func apps(_ request: String) async -> Output {
        let r = request.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !r.isEmpty else { return Output(text: "No request given.", error: true) }
        let prompt = """
        \(r)

        (A voice assistant is asking this for the user, in their connected apps. Do exactly what's asked and nothing \
        more, then reply in one to three short plain sentences to be read aloud: no markdown, lists or links. If \
        something you need is missing or unclear, say so instead of guessing.)
        """
        let allowed = connectors.map { "mcp__claude_ai_\($0)" }.joined(separator: ",")
        let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("Bobble")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return await withCheckedContinuation { cont in
            DispatchQueue.global().async {
                let p = Process(), out = Pipe(), err = Pipe()
                p.executableURL = URL(fileURLWithPath: "/bin/zsh")
                // (a login shell, so `claude` is on the PATH as usual; the request is an argument, never part of the command)
                // (Sonnet: ~5 s a request, where the default model took 8 to 18; low effort was no quicker and
                // sometimes missed the connector)
                p.arguments = ["-lc", "exec claude -p \"$1\" --model sonnet --output-format text --max-turns 12 " +
                               "--settings '{\"disableAllHooks\":true}' --allowedTools '\(allowed)'", "zsh", prompt]
                p.currentDirectoryURL = dir
                p.standardInput = FileHandle.nullDevice
                p.standardOutput = out
                p.standardError = err
                do { try p.run() } catch {
                    cont.resume(returning: Output(text: "Couldn't start Claude Code: \(error.localizedDescription)", error: true))
                    return
                }
                let limit = DispatchWorkItem { if p.isRunning { p.terminate() } }
                DispatchQueue.global().asyncAfter(deadline: .now() + 150, execute: limit)
                let o = out.fileHandleForReading.readDataToEndOfFile()
                let e = err.fileHandleForReading.readDataToEndOfFile()
                p.waitUntilExit()
                limit.cancel()
                let text = String(decoding: o, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
                if p.terminationReason == .uncaughtSignal {
                    cont.resume(returning: Output(text: "Claude Code took too long (over two and a half minutes) and was stopped.", error: true))
                } else if text.isEmpty {
                    let why = String(decoding: e, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines).prefix(300)
                    cont.resume(returning: Output(text: "Claude Code didn't answer\(why.isEmpty ? "" : ": \(why)")", error: true))
                } else {
                    cont.resume(returning: Output(text: text))
                }
            }
        }
    }
}
