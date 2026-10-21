// Bobble: the Mac app for the Bobble display, in the menu bar (no Dock icon). Hook events come in from Claude Code (HookServer), become each chat's
// state (Chats), and go out to the display (Device). The menu shows the display, the chats, and the settings.
import AppKit
import ServiceManagement

final class App: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private let menu = NSMenu()
    private let chats = Chats()
    private let device = Device()
    private let sims = [Device(sim: Device.bubble), Device(sim: Device.square)]   // the simulators, when they're running (chats go to all)
    private var server: HookServer!
    private let updater = Updater()              // new builds from the Bobble repo's releases
    private var badged = false                    // the menu-bar mark has its "needs you" dot

    func applicationDidFinishLaunching(_ note: Notification) {
        item.button?.image = App.menuIcon(badge: false)
        menu.delegate = self
        menu.autoenablesItems = false             // (chat rows have no action but should read as normal text, not greyed)
        item.menu = menu
        chats.hideText = UserDefaults.standard.bool(forKey: "hideText")
        if !UserDefaults.standard.bool(forKey: "loginDefaulted") {     // (open at login by default, once; the menu can turn it off)
            UserDefaults.standard.set(true, forKey: "loginDefaulted")
            if SMAppService.mainApp.status != .enabled { try? SMAppService.mainApp.register() }
        }
        chats.onUpdate = { [weak self] c in
            guard let self else { return }
            let p = self.chats.payload(c)
            self.device.send(chat: c.id, p)
            for s in self.sims { s.send(chat: c.id, p) }
            self.refreshIcon()
        }
        device.onChange = { [weak self] in self?.refreshIcon() }
        server = HookServer { [weak self] path, j in
            if path.hasPrefix("/cursor") { self?.chats.handleCursor(j) }
            else if path.hasPrefix("/codex") { self?.chats.handle(j, agent: "codex") }
            else { self?.chats.handle(j) }
        }
        do { try server.start() } catch { NSLog("can't listen for hooks: \(error)") }
        device.start()
        for s in sims {
            s.onChange = { [weak self, weak s] in       // (a simulator that's just started, or restarted, has no chats yet)
                guard let self, let s, case .found = s.status else { return }
                for c in self.chats.all.values where c.state != .idle { s.send(chat: c.id, self.chats.payload(c)) }
            }
            s.start()
        }
        updater.start()
        refreshIcon()
        Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in self?.chats.check() }   // (interrupts)
    }

    // The menu-bar mark: a solid round face with its two resting eyes cut out. A template image, so macOS tints it for
    // light and dark menu bars (and dims it while the display's away). badge: a dot top right, cut clear of the face,
    // while a chat needs you.
    static func menuIcon(badge: Bool) -> NSImage {
        let img = NSImage(size: NSSize(width: 18, height: 18), flipped: false) { _ in
            let face = NSBezierPath(ovalIn: NSRect(x: 1, y: 1, width: 16, height: 16))
            for x in [5.8, 10.0] {                   // the eyes: holes, so the menu bar shows through
                face.append(NSBezierPath(roundedRect: NSRect(x: x, y: 7, width: 2.4, height: 4.6), xRadius: 1.2, yRadius: 1.2).reversed)
            }
            NSColor.black.setFill()
            face.fill()
            if badge {
                NSGraphicsContext.current?.compositingOperation = .clear
                NSBezierPath(ovalIn: NSRect(x: 10.5, y: 10.5, width: 8, height: 8)).fill()
                NSGraphicsContext.current?.compositingOperation = .sourceOver
                NSBezierPath(ovalIn: NSRect(x: 12, y: 12, width: 5.5, height: 5.5)).fill()
            }
            return true
        }
        img.isTemplate = true
        img.accessibilityDescription = "Bobble"
        return img
    }

    private func refreshIcon() {
        let waiting = chats.all.values.contains { $0.state == .waiting }
        if waiting != badged { badged = waiting; item.button?.image = App.menuIcon(badge: waiting) }
        if case .online = device.status { item.button?.appearsDisabled = false } else { item.button?.appearsDisabled = true }
    }

    // ---- the menu, rebuilt each time it opens: the display, the chats (the ones that need you first), where chats
    // come from, then settings
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        let (status, symbol, color) = deviceLine
        menu.addItem(row(status, sub: deviceHint, symbol: symbol, color: color))
        addDeviceItems(device)
        menu.addItem(.separator())
        for s in sims {
            if case .searching = s.status {
                let open = action("Open \(s.sim!.name)", #selector(openSimulator(_:)))
                open.representedObject = s
                menu.addItem(open)
            } else {
                menu.addItem(header(s.sim!.name))
                addDeviceItems(s)
            }
        }
        menu.addItem(.separator())

        let active = chats.all.values.filter { $0.state != .idle }
            .sorted { ($0.state == .waiting ? 0 : 1, $1.changed) < ($1.state == .waiting ? 0 : 1, $0.changed) }
        let need = active.filter { $0.state == .waiting }.count, busy = active.filter { $0.state == .working }.count
        let counts = [need > 0 ? "\(need) need\(need == 1 ? "s" : "") you" : "", busy > 0 ? "\(busy) working" : ""].filter { !$0.isEmpty }
        menu.addItem(header(counts.isEmpty ? "Chats" : "Chats · " + counts.joined(separator: ", ")))
        if active.isEmpty { menu.addItem(note("No chats right now")) }
        for c in active { menu.addItem(chatRow(c)) }
        menu.addItem(.separator())

        menu.addItem(header("Show Chats From"))
        menu.addItem(source("Claude Code", on: Hooks.installed(), #selector(installHooks), #selector(removeHooks)))
        if CodexHooks.codexInstalled { menu.addItem(source("Codex", on: CodexHooks.installed(), #selector(installCodex), #selector(removeCodex))) }
        if CursorHooks.cursorInstalled { menu.addItem(source("Cursor", on: CursorHooks.installed(), #selector(installCursor), #selector(removeCursor))) }
        menu.addItem(.separator())

        let hide = action("Hide Message Text on Display", #selector(toggleHide))
        hide.state = chats.hideText ? .on : .off
        menu.addItem(hide)
        let login = action("Open at Login", #selector(toggleLogin))
        login.state = SMAppService.mainApp.status == .enabled ? .on : .off
        menu.addItem(login)
        menu.addItem(updateItem)
        menu.addItem(note("Build \(Updater.build)"))
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Quit Bobble", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
    }

    // Pair, Calendars and Spotify for one display (each item knows which: the board, or the simulator)
    private func addDeviceItems(_ d: Device) {
        let pair = action(d.paired ? "Pair Again…" : (d.simulator ? "Pair…" : "Pair with Display…"), #selector(pair(_:)))
        if case .searching = d.status { pair.isEnabled = false }
        let cals = action("Calendars…", #selector(calendars(_:)))
        if case .online = d.status {} else if case .found = d.status {} else { cals.isEnabled = false }
        let rename = action("Rename…", #selector(rename(_:)))
        rename.isEnabled = d.paired && cals.isEnabled
        let spot = NSMenuItem(title: d.spotifyConnected == true ? "Disconnect Spotify" : "Connect Spotify…",
                              action: d.spotifyConnected == true ? #selector(disconnectSpotify(_:)) : #selector(connectSpotify(_:)), keyEquivalent: "")
        spot.target = self
        spot.isEnabled = cals.isEnabled
        for i in [pair, rename, cals, spot] { i.representedObject = d; menu.addItem(i) }
        d.refreshSpotify()                            // (keeps the item right for next time)
    }

    private var deviceLine: (String, String, NSColor) {
        switch device.status {
        case .searching: return ("Looking for a display…", "magnifyingglass", .secondaryLabelColor)
        case .found(let n): return (n, "circle.fill", .systemYellow)
        case .online(let n): return (n, "circle.fill", .systemGreen)
        case .unreachable(let n): return (n, "exclamationmark.triangle.fill", .systemOrange)
        case .unpaired(let n): return (n, "link.badge.plus", .systemBlue)
        case .several(let n): return ("\(n) Bobbles nearby", "dot.radiowaves.left.and.right", .systemBlue)
        }
    }

    private var deviceHint: String {
        switch device.status {
        case .searching: return "Make sure it's on and on this Wi-Fi"
        case .found: return "Found, connecting…"
        case .online: return "Connected"
        case .unreachable: return "Not responding, retrying"
        case .unpaired: return "Needs pairing"
        case .several: return "Pair to choose yours"
        }
    }

    private static let chatLook: [ChatState: (String, NSColor, String)] = [
        .waiting: ("exclamationmark.circle.fill", .systemOrange, "Needs you"),
        .working: ("ellipsis.circle.fill", .systemBlue, "Working"),
        .done: ("checkmark.circle.fill", .systemGreen, "Done"),
        .stopped: ("stop.circle.fill", .secondaryLabelColor, "Stopped"),
    ]

    private func chatRow(_ c: Chat) -> NSMenuItem {
        let (symbol, color, label) = App.chatLook[c.state] ?? ("circle", .secondaryLabelColor, "")
        let sub = [c.detail.isEmpty ? label : c.detail, App.ago(c.changed)].joined(separator: " · ")
        let it = row(c.title.isEmpty ? c.project : c.title, sub: sub, symbol: symbol, color: color)
        it.toolTip = [c.project, c.model].filter { !$0.isEmpty }.joined(separator: " · ")
        return it
    }

    private static let relative: RelativeDateTimeFormatter = { let f = RelativeDateTimeFormatter(); f.unitsStyle = .short; return f }()
    private static func ago(_ d: Date) -> String {
        Date().timeIntervalSince(d) < 60 ? "just now" : relative.localizedString(for: d, relativeTo: Date())
    }

    // A two-line row: a name, and under it a smaller grey line, with a coloured symbol. No action, but enabled so it
    // reads as normal text.
    private func row(_ title: String, sub: String, symbol: String, color: NSColor) -> NSMenuItem {
        let s = NSMutableAttributedString(string: clip(title, 46), attributes: [.font: NSFont.menuFont(ofSize: 0)])
        s.append(NSAttributedString(string: "\n" + clip(sub, 56), attributes: [
            .font: NSFont.menuFont(ofSize: NSFont.smallSystemFontSize), .foregroundColor: NSColor.secondaryLabelColor]))
        let it = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        it.attributedTitle = s
        it.image = NSImage(systemSymbolName: symbol, accessibilityDescription: nil)?
            .withSymbolConfiguration(.init(pointSize: 15, weight: .regular).applying(.init(hierarchicalColor: color)))
        return it
    }

    private func header(_ title: String) -> NSMenuItem {
        let it = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        it.attributedTitle = NSAttributedString(string: title, attributes: [
            .font: NSFont.systemFont(ofSize: NSFont.smallSystemFontSize, weight: .semibold), .foregroundColor: NSColor.secondaryLabelColor])
        it.isEnabled = false
        return it
    }

    private func note(_ title: String) -> NSMenuItem {
        let it = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        it.isEnabled = false
        return it
    }

    private func action(_ title: String, _ sel: Selector) -> NSMenuItem {
        let it = NSMenuItem(title: title, action: sel, keyEquivalent: "")
        it.target = self
        return it
    }

    // An agent to show chats from, ticked while its hooks are in: choosing it connects (asking first) or disconnects
    private func source(_ name: String, on: Bool, _ add: Selector, _ remove: Selector) -> NSMenuItem {
        let it = action(name, on ? remove : add)
        it.state = on ? .on : .off
        it.toolTip = on ? "Connected. Choose to disconnect." : "Choose to connect."
        return it
    }

    private func clip(_ s: String, _ n: Int) -> String { s.count > n ? String(s.prefix(n - 1)) + "…" : s }

    // A simulator: open it if it's installed (~/esp/amoled_demo/sim/build_app.sh puts them in ~/Applications)
    @objc private func openSimulator(_ sender: NSMenuItem) {
        guard let name = target(sender).sim?.name else { return }
        let app = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Applications/\(name).app")
        guard FileManager.default.fileExists(atPath: app.path) else {
            say("The simulator isn't installed", "Build it with ~/esp/amoled_demo/sim/build_app.sh.")
            return
        }
        NSWorkspace.shared.openApplication(at: app, configuration: NSWorkspace.OpenConfiguration())
    }

    @objc private func toggleHide() {
        chats.hideText.toggle()
        UserDefaults.standard.set(chats.hideText, forKey: "hideText")
        for c in chats.all.values where c.state != .idle {   // (re-send as it is now)
            device.send(chat: c.id, chats.payload(c))
            for s in sims { s.send(chat: c.id, chats.payload(c)) }
        }
    }

    @objc private func installHooks() {
        let a = NSAlert()
        a.messageText = "Connect to Claude Code?"
        a.informativeText = "Bobble adds a small hook to ~/.claude/settings.json so every Claude Code chat on this Mac shows on your display. Your other settings stay as they are, and a backup is kept next to the file. New chats pick it up straight away; chats already open pick it up when restarted."
        a.addButton(withTitle: "Connect")
        a.addButton(withTitle: "Cancel")
        NSApp.activate(ignoringOtherApps: true)
        guard a.runModal() == .alertFirstButtonReturn else { return }
        do { try Hooks.install() } catch { report("Couldn't update Claude Code's settings", error) }
    }

    @objc private func installCursor() {
        let a = NSAlert()
        a.messageText = "Connect to Cursor?"
        a.informativeText = "Bobble adds hooks to ~/.cursor/hooks.json so Cursor's agent chats show on your display (a backup is kept next to the file). They only report what happened; they never block or approve anything. Cursor picks them up after a restart."
        a.addButton(withTitle: "Connect")
        a.addButton(withTitle: "Cancel")
        NSApp.activate(ignoringOtherApps: true)
        guard a.runModal() == .alertFirstButtonReturn else { return }
        do { try CursorHooks.install() } catch { report("Couldn't update Cursor's hooks", error) }
    }

    @objc private func installCodex() {
        let a = NSAlert()
        a.messageText = "Connect to Codex?"
        a.informativeText = "Bobble adds hooks to ~/.codex/hooks.json so Codex chats (including Codex in Conductor) show on your display. A backup is kept next to the file.\n\nCodex runs a new hook only after you trust it: open Codex in a terminal, type /hooks, and trust the Bobble hooks once."
        a.addButton(withTitle: "Connect")
        a.addButton(withTitle: "Cancel")
        NSApp.activate(ignoringOtherApps: true)
        guard a.runModal() == .alertFirstButtonReturn else { return }
        do { try CodexHooks.install() } catch { report("Couldn't update Codex's hooks", error) }
    }

    @objc private func removeCodex() {
        do { try CodexHooks.remove() } catch { report("Couldn't update Codex's hooks", error) }
    }

    @objc private func removeCursor() {
        do { try CursorHooks.remove() } catch { report("Couldn't update Cursor's hooks", error) }
    }

    @objc private func removeHooks() {
        do { try Hooks.remove() } catch { report("Couldn't update Claude Code's settings", error) }
    }

    // Pairing: the display puts up a code; type it here and the display gives this Mac a key of its own
    @objc private func pair(_ sender: NSMenuItem) {
        let device = target(sender)
        var board: Device.Board?
        if !device.simulator && device.boards.count > 1 {   // several Bobbles about: which one's yours?
            let a = NSAlert()
            a.messageText = "Which Bobble is yours?"
            a.informativeText = "Each Bobble shows its name on the pairing screen and under Settings › Factory reset. A code will appear on the one you pick."
            let pop = NSPopUpButton(frame: NSRect(x: 0, y: 0, width: 240, height: 26))
            for b in device.boards { pop.addItem(withTitle: b.name + (b.id != nil && b.id == device.displayID ? "  (yours)" : "")) }
            a.accessoryView = pop
            a.addButton(withTitle: "Show Code")
            a.addButton(withTitle: "Cancel")
            NSApp.activate(ignoringOtherApps: true)
            guard a.runModal() == .alertFirstButtonReturn else { return }
            board = device.boards[pop.indexOfSelectedItem]
        } else if !device.simulator, device.boards.count == 1 {
            board = device.boards[0]
        }
        device.startPairing(board) { [weak self] ok in
            guard let self else { return }
            guard ok else { self.say("Couldn't reach the display", "Make sure it's on and on the same Wi-Fi as this Mac."); return }
            self.askCode("Look at \(board?.name ?? "your display"): it's showing a 6-digit code.", device)
        }
    }

    // The board, or the simulator for the items in its own section
    private func target(_ sender: NSMenuItem) -> Device { sender.representedObject as? Device ?? device }

    private func askCode(_ text: String, _ device: Device) {
        let a = NSAlert()
        a.messageText = "Pair with display"
        a.informativeText = text
        let f = NSTextField(frame: NSRect(x: 0, y: 0, width: 160, height: 28))
        f.placeholderString = "123 456"
        f.font = .monospacedDigitSystemFont(ofSize: 18, weight: .medium)
        a.accessoryView = f
        a.addButton(withTitle: "Pair")
        a.addButton(withTitle: "Cancel")
        a.window.initialFirstResponder = f
        NSApp.activate(ignoringOtherApps: true)
        guard a.runModal() == .alertFirstButtonReturn else { device.cancelPairing(); return }
        let code = f.stringValue.filter(\.isNumber)
        device.finishPairing(code: code) { [weak self] r in
            switch r {
            case .paired: self?.say("Paired", "Your chats will show on the display.")
            case .wrongCode: self?.askCode("That wasn't the code on the display. Try again.", device)
            case .expired: self?.say("The code ran out", "Choose Pair with display again for a new one.")
            case .failed: self?.say("Couldn't pair", "The display didn't answer. Try again in a moment.")
            }
        }
    }

    // A name for it, shown in this menu, on its pairing screen and to other Macs choosing theirs
    @objc private func rename(_ sender: NSMenuItem) {
        let device = target(sender)
        let a = NSAlert()
        a.messageText = "Rename \(device.name)"
        a.informativeText = "Leave it empty to go back to its own name."
        let f = NSTextField(frame: NSRect(x: 0, y: 0, width: 240, height: 24))
        f.stringValue = device.name
        a.accessoryView = f
        a.addButton(withTitle: "Rename")
        a.addButton(withTitle: "Cancel")
        a.window.initialFirstResponder = f
        NSApp.activate(ignoringOtherApps: true)
        guard a.runModal() == .alertFirstButtonReturn else { return }
        device.rename(String(f.stringValue.prefix(31))) { [weak self] n in
            if n == nil { self?.say("Couldn't rename it", "The display didn't answer. Try again in a moment.") }
        }
    }

    // Calendars: their private .ics links, one per line (Google Calendar: Settings > the calendar > "Secret address in
    // iCal format"; Apple Calendar: share the calendar publicly and copy the link)
    @objc private func calendars(_ sender: NSMenuItem) {
        let device = target(sender)
        device.getCalendars { [weak self] urls in
            guard let self else { return }
            guard let urls else { self.say("Couldn't reach the display", "Make sure it's on and paired."); return }
            let a = NSAlert()
            a.messageText = "Calendars"
            a.informativeText = "Paste each calendar's private link (.ics), one per line. Google Calendar: Settings › your calendar › Secret address in iCal format. Apple Calendar: share it as a public calendar and copy the link."
            let scroll = NSScrollView(frame: NSRect(x: 0, y: 0, width: 420, height: 120))
            scroll.hasVerticalScroller = true
            let text = NSTextView(frame: scroll.bounds)
            text.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
            text.isRichText = false
            text.string = urls.joined(separator: "\n")
            text.autoresizingMask = [.width]
            scroll.documentView = text
            a.accessoryView = scroll
            a.addButton(withTitle: "Save")
            a.addButton(withTitle: "Cancel")
            NSApp.activate(ignoringOtherApps: true)
            guard a.runModal() == .alertFirstButtonReturn else { return }
            let list = text.string.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
            device.setCalendars(list) { n in
                if n == nil { self.say("Couldn't update the calendars", "The display didn't take them. Try again in a moment.") }
            }
        }
    }

    // Spotify: sign in in the browser; the display gets its own access and plays from it
    @objc private func connectSpotify(_ sender: NSMenuItem) {
        Spotify.connect(target(sender)) { [weak self] result in
            switch result {
            case .success: self?.say("Spotify connected", "Swipe to the music page on your Bobble.")
            case .failure(let e): self?.say("Couldn't connect Spotify", "\(e)")
            }
        }
    }

    @objc private func disconnectSpotify(_ sender: NSMenuItem) {
        target(sender).disconnectSpotify { [weak self] ok in
            if !ok { self?.say("Couldn't disconnect Spotify", "The display didn't answer. Try again in a moment.") }
        }
    }

    // "Check for Updates…", saying where things stand (the build number in its tooltip)
    private var updateItem: NSMenuItem {
        let title: String
        switch updater.status {
        case .checking: title = "Checking for Updates…"
        case .installing(let n): title = "Installing Build \(n)…"
        default: title = "Check for Updates…"
        }
        let it = action(title, #selector(checkUpdates))
        if case .checking = updater.status { it.isEnabled = false }
        if case .installing = updater.status { it.isEnabled = false }
        it.toolTip = "Bobble build \(Updater.build)"
        return it
    }

    @objc private func checkUpdates() {
        updater.onChange = { [weak self] in
            guard let self else { return }
            switch self.updater.status {
            case .upToDate: self.say("Bobble is up to date", "This is build \(Updater.build), the newest.")
            case .failed(let why): self.say("Couldn't update Bobble", why)
            default: return
            }
            self.updater.onChange = {}               // (only for this check: the background ones stay quiet)
        }
        updater.check()
    }

    @objc private func toggleLogin() {
        do {
            if SMAppService.mainApp.status == .enabled { try SMAppService.mainApp.unregister() }
            else { try SMAppService.mainApp.register() }
        } catch { report("Couldn't change Open at login", error) }
    }

    private func say(_ title: String, _ text: String) {
        let a = NSAlert()
        a.messageText = title
        a.informativeText = text
        NSApp.activate(ignoringOtherApps: true)
        a.runModal()
    }

    private func report(_ what: String, _ error: Error) {
        let a = NSAlert(error: error)
        a.messageText = what
        NSApp.activate(ignoringOtherApps: true)
        a.runModal()
    }
}

let app = NSApplication.shared
let delegate = App()
app.delegate = delegate
app.setActivationPolicy(.accessory)               // menu bar only
app.run()
