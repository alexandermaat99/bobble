// The agents' hooks for the bridge. Each event the display cares about runs one tiny command that hands the event to
// this app (and never holds the agent up, even when the app isn't running). Installing replaces older display hooks
// (the prototype's agent_hook.sh); everything else in the agent's settings is left as it was, with a backup beside it.
import Foundation

// Claude Code's, in ~/.claude/settings.json
enum Hooks {
    static let events = ["UserPromptSubmit", "PreToolUse", "PostToolUse", "PermissionRequest", "Notification", "Stop", "SessionEnd"]
    static let marker = "AGENT_BRIDGE=1"
    // PermissionRequest waits for an answer tapped on the display and prints what the app says (nothing, if the app
    // isn't running or nobody taps: Claude Code then asks in the terminal as usual)
    static func command(_ event: String) -> String {
        event == "PermissionRequest"
            ? "\(marker) curl -s -m 90 --data-binary @- http://127.0.0.1:\(HookServer.port)/permission 2>/dev/null || true"
            : "\(marker) curl -s -m 1 --data-binary @- http://127.0.0.1:\(HookServer.port)/hook >/dev/null 2>&1 || true"
    }
    static var settingsURL: URL { FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".claude/settings.json") }

    static func installed() -> Bool { installed(at: settingsURL, events) }
    private static func asksInstalled() -> Bool {
        (((load(settingsURL)["hooks"] as? [String: Any])?["PreToolUse"] as? [[String: Any]]) ?? [])
            .contains { $0["matcher"] as? String == "AskUserQuestion" }
    }
    // (and for AskUserQuestion, a second PreToolUse hook that waits for its answer tapped on the display)
    static func install() throws {
        try update(settingsURL, events, adding: true, command: command) { e in
            e == "PreToolUse" ? [["matcher": "AskUserQuestion",
                                  "hooks": [["type": "command", "command": command("PermissionRequest"), "timeout": 120]]]] : []
        }
    }
    static func remove() throws { try update(settingsURL, events, adding: false, command: command) }
    // Connected before an event was added (ours on some, not all): brought up to date
    static func upgrade() {
        guard !installed() || !asksInstalled(), installed(at: settingsURL, ["Stop"]) else { return }
        try? install()
    }

    // Ours, or the prototype's script
    private static func isOurs(_ h: [String: Any]) -> Bool {
        let c = h["command"] as? String ?? ""
        return c.contains(marker) || c.hasSuffix("agent_hook.sh")
    }

    static func load(_ url: URL) -> [String: Any] {
        guard let d = try? Data(contentsOf: url) else { return [:] }
        return (try? JSONSerialization.jsonObject(with: d)) as? [String: Any] ?? [:]
    }

    // In a Claude Code-style file ({"hooks": {event: [{"hooks": [{"type", "command"}]}]}}): ours on every event?
    static func installed(at url: URL, _ events: [String]) -> Bool {
        guard let hooks = load(url)["hooks"] as? [String: Any] else { return false }
        return events.allSatisfy { e in
            ((hooks[e] as? [[String: Any]]) ?? []).contains { g in
                ((g["hooks"] as? [[String: Any]]) ?? []).contains { ($0["command"] as? String ?? "").contains(marker) }
            }
        }
    }

    // Adds (or takes out) our hook for each event in a Claude Code-style file
    static func update(_ url: URL, _ events: [String], adding: Bool, command: (String) -> String,
                       also: (String) -> [[String: Any]] = { _ in [] }) throws {
        var settings = load(url)
        var hooks = settings["hooks"] as? [String: Any] ?? [:]
        for e in events {
            // drop our entries (and the old script's) from every matcher group, and groups left empty by that
            var groups = ((hooks[e] as? [[String: Any]]) ?? []).compactMap { g -> [String: Any]? in
                var g = g
                let kept = ((g["hooks"] as? [[String: Any]]) ?? []).filter { !isOurs($0) }
                if kept.isEmpty { return nil }
                g["hooks"] = kept
                return g
            }
            if adding { groups.append(["hooks": [["type": "command", "command": command(e), "timeout": e == "PermissionRequest" ? 120 : 5]]]); groups += also(e) }
            hooks[e] = groups.isEmpty ? nil : groups
        }
        settings["hooks"] = hooks.isEmpty ? nil : hooks
        try write(settings, to: url)
    }

    // Writes a settings file, keeping a copy of how it was next to it
    static func write(_ json: [String: Any], to url: URL) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        if fm.fileExists(atPath: url.path) {
            let bak = url.appendingPathExtension("agentbridge.bak")
            try? fm.removeItem(at: bak)
            try fm.copyItem(at: url, to: bak)
        }
        let data = try JSONSerialization.data(withJSONObject: json, options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes])
        try data.write(to: url, options: .atomic)
    }
}

// Codex's, in ~/.codex/hooks.json: the same format as Claude Code's (OpenAI modelled them on it). Stop has to answer
// with JSON, so its command prints {}. Codex only runs a new hook once you've trusted it (/hooks in Codex).
enum CodexHooks {
    static let events = ["UserPromptSubmit", "PreToolUse", "PostToolUse", "PermissionRequest", "Stop", "SessionEnd"]
    static var url: URL { FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".codex/hooks.json") }
    static var codexInstalled: Bool { FileManager.default.fileExists(atPath: url.deletingLastPathComponent().path) }

    static func command(_ event: String) -> String {
        "\(Hooks.marker) curl -s -m 1 --data-binary @- http://127.0.0.1:\(HookServer.port)/codex >/dev/null 2>&1" +
            (event == "Stop" ? "; echo '{}'" : " || true")
    }

    static func installed() -> Bool { Hooks.installed(at: url, events) }
    static func install() throws { try Hooks.update(url, events, adding: true, command: command) }
    static func remove() throws { try Hooks.update(url, events, adding: false, command: command) }
}

// Cursor's, in ~/.cursor/hooks.json ({"version": 1, "hooks": {event: [{"command": ...}]}}). Only events that can't block
// anything (a "before" hook that doesn't answer properly stops the action). The command always prints
// {"continue": true}, so Cursor never reads it as a failure.
enum CursorHooks {
    static let events = ["beforeSubmitPrompt", "postToolUse", "afterShellExecution", "afterMCPExecution", "afterFileEdit",
                         "afterAgentResponse", "stop", "sessionEnd"]
    static let command = "\(Hooks.marker) curl -s -m 1 --data-binary @- http://127.0.0.1:\(HookServer.port)/cursor >/dev/null 2>&1; echo '{\"continue\":true}'"
    static var url: URL { FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".cursor/hooks.json") }
    static var cursorInstalled: Bool { FileManager.default.fileExists(atPath: "/Applications/Cursor.app") ||
                                       FileManager.default.fileExists(atPath: url.deletingLastPathComponent().path) }

    static func installed() -> Bool {
        guard let hooks = Hooks.load(url)["hooks"] as? [String: Any] else { return false }
        return events.allSatisfy { e in ((hooks[e] as? [[String: Any]]) ?? []).contains { ($0["command"] as? String ?? "").contains(Hooks.marker) } }
    }

    static func install() throws { try update(adding: true) }
    static func remove() throws { try update(adding: false) }

    private static func update(adding: Bool) throws {
        var file = Hooks.load(url)
        var hooks = file["hooks"] as? [String: Any] ?? [:]
        for e in events {
            var list = ((hooks[e] as? [[String: Any]]) ?? []).filter { !($0["command"] as? String ?? "").contains(Hooks.marker) }
            if adding { list.append(["command": command, "timeout": 5]) }
            hooks[e] = list.isEmpty ? nil : list
        }
        file["version"] = file["version"] ?? 1
        file["hooks"] = hooks
        try Hooks.write(file, to: url)
    }
}
