// Bobble Bubble / Bobble Square (the simulators): the display's glass (round, or the square board's) floating on the desktop (no window around it), with the firmware (main/,
// unchanged) running behind it. The mouse is a finger; the Device menu turns, lays down and picks up the board, presses
// BOOT, and sets the battery.
//   click/drag on the glass: touch      drag the rim (or ⌘-drag anywhere): move it      ⌘+ / ⌘- / ⌘9 (tiny): size
//   B (held): BOOT                      [ and ]: turn 90°                                F: lay flat / stand up
import AppKit
import Firmware

// The glass this build of the firmware draws: Bobble Bubble, 466 round (the AMOLED), or Bobble Square, 480 square (the
// 4" LCD: BOARD_SQUARE). Two apps, so both can run at once.
let side = Int(sim_glass_side()), round = sim_glass_round()
let appName = round ? "Bobble Bubble" : "Bobble Square"

final class GlassView: NSView {
    private var raw = [UInt16](repeating: 0, count: side * side)
    private var rgba = [UInt8](repeating: 0, count: side * side * 4)
    private var mask = [Float](repeating: 1, count: side * side)   // the round glass, with a soft edge (square: all of it)
    private var seq: UInt32 = 0
    private var lastLook = (bright: -1, on: false)
    private var image: CGImage?

    override init(frame: NSRect) {
        super.init(frame: frame)
        let n = side, c = Float(n) / 2
        for y in 0..<n where round {
            for x in 0..<n {
                let dx = Float(x) + 0.5 - c, dy = Float(y) + 0.5 - c
                mask[y * n + x] = max(0, min(1, c - (dx * dx + dy * dy).squareRoot()))
            }
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    // Called on the screen's refresh: picks up whatever the firmware drew since last time
    func refresh() {
        let s = raw.withUnsafeMutableBufferPointer { sim_copy_glass($0.baseAddress, seq) }
        let look = (bright: Int(sim_brightness()), on: sim_panel_on())
        guard s != seq || look != lastLook else { return }
        seq = s
        lastLook = look
        let k = look.on ? Float(look.bright) / 255 : 0   // (on an AMOLED, brightness scales every pixel)
        let glow: Float = round ? 0 : 0.03                // (an LCD's backlight shows through its black)
        let n = side * side
        raw.withUnsafeBufferPointer { src in
            rgba.withUnsafeMutableBufferPointer { dst in
                for i in 0..<n {
                    let v = src[i].byteSwapped, m = mask[i] * k   // (the panel takes big-endian RGB565)
                    dst[i * 4] = UInt8((Float(Int(v >> 11) * 255 / 31) * (1 - glow) + 255 * glow) * m)
                    dst[i * 4 + 1] = UInt8((Float(Int((v >> 5) & 63) * 255 / 63) * (1 - glow) + 255 * glow) * m)
                    dst[i * 4 + 2] = UInt8((Float(Int(v & 31) * 255 / 31) * (1 - glow) + 255 * glow) * m)
                    dst[i * 4 + 3] = UInt8(255 * mask[i])
                }
            }
        }
        let w = side
        image = CGImage(width: w, height: w, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                        provider: CGDataProvider(data: Data(rgba) as CFData)!, decode: nil, shouldInterpolate: true,
                        intent: .defaultIntent)
        needsDisplay = true
    }

    static let rim: CGFloat = 4                       // the board's dark edge round the glass: the handle to move it by

    private var glassRect: NSRect { bounds.insetBy(dx: GlassView.rim, dy: GlassView.rim) }

    override func draw(_ dirty: NSRect) {
        NSColor.clear.setFill()
        bounds.fill()
        let g = glassRect
        let r = bounds.width * 0.06                   // (the square board's corners)
        NSColor(white: 0.13, alpha: 1).setFill()      // the rim
        (round ? NSBezierPath(ovalIn: bounds) : NSBezierPath(roundedRect: bounds, xRadius: r, yRadius: r)).fill()
        let gr = max(0, r - GlassView.rim)            // (the glass follows the rim's corners, inside it)
        let glass = round ? NSBezierPath(ovalIn: g) : NSBezierPath(roundedRect: g, xRadius: gr, yRadius: gr)
        NSColor.black.setFill()
        glass.fill()
        guard let image, let cg = NSGraphicsContext.current?.cgContext else { return }
        glass.addClip()
        cg.interpolationQuality = .high
        cg.draw(image, in: g)
    }

    // ---- the mouse is a finger
    private func glassPoint(_ e: NSEvent) -> (Int32, Int32) {
        let p = convert(e.locationInWindow, from: nil), g = glassRect, k = CGFloat(side) / g.width
        let x = (p.x - g.minX) * k, y = (g.maxY - p.y) * k
        return (Int32(max(0, min(CGFloat(side - 1), x))), Int32(max(0, min(CGFloat(side - 1), y))))
    }
    // On the rim, or with ⌘: moves the window. On the glass: a finger.
    private var touching = false
    override func mouseDown(with e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil), g = glassRect
        let onGlass = round ? hypot(p.x - g.midX, p.y - g.midY) <= g.width / 2 : g.contains(p)
        if !onGlass || e.modifierFlags.contains(.command) { window?.performDrag(with: e); return }
        touching = true
        let (x, y) = glassPoint(e)
        sim_touch(x, y, true)
    }
    override func mouseDragged(with e: NSEvent) { guard touching else { return }; let (x, y) = glassPoint(e); sim_touch(x, y, true) }
    override func mouseUp(with e: NSEvent) {
        guard touching else { return }
        touching = false
        let (x, y) = glassPoint(e)
        sim_touch(x, y, false)
    }

    // Round: clicks beside the circle go to whatever's behind
    override func hitTest(_ p: NSPoint) -> NSView? {
        let q = convert(p, from: superview)
        return !round || hypot(q.x - bounds.midX, q.y - bounds.midY) <= bounds.width / 2 ? self : nil
    }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    // ---- keys: B is BOOT (held as long as the key is), [ ] turn the board, F lays it down
    override func keyDown(with e: NSEvent) {
        guard !e.isARepeat else { return }
        switch e.charactersIgnoringModifiers?.lowercased() {
        case "b": sim_boot_button(true)
        case "[": App.shared.turn(-90)
        case "]": App.shared.turn(90)
        case "f": App.shared.toggleFlat()
        default: super.keyDown(with: e)
        }
    }
    override func keyUp(with e: NSEvent) {
        if e.charactersIgnoringModifiers?.lowercased() == "b" { sim_boot_button(false) }
    }
}

// A borderless window that still takes the keyboard (BOOT and the turning keys)
final class GlassWindow: NSWindow {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
}

final class App: NSObject, NSApplicationDelegate {
    static let shared = App()
    private var window: NSWindow!
    private let view = GlassView(frame: NSRect(x: 0, y: 0, width: 480, height: 480))
    private var tilt: Float = UserDefaults.standard.object(forKey: "tilt") as? Float ?? 0
    private var flat = UserDefaults.standard.bool(forKey: "flat")

    func applicationDidFinishLaunching(_ n: Notification) {
        window = GlassWindow(contentRect: view.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.title = appName
        window.isOpaque = false
        window.backgroundColor = .clear
        window.hasShadow = true
        window.level = UserDefaults.standard.object(forKey: "onTop") as? Bool ?? true ? .floating : .normal
        window.collectionBehavior = [.fullScreenAuxiliary]
        window.contentView = view
        // Restarts reopen it where it was, at its size (kept by hand: AppKit restores only the position of a window that
        // can't be resized). First launch: Tiny, bottom left.
        if let f = UserDefaults.standard.string(forKey: "frame").map(NSRectFromString), f.width > 0 {
            window.setFrame(f, display: false)
        } else if let s = NSScreen.main?.visibleFrame {
            let x = s.minX + (round ? 10 : 20 + App.tinySide)   // (Square beside Bubble)
            window.setFrame(NSRect(x: x, y: s.minY + 15, width: App.tinySide, height: App.tinySide), display: false)
        }
        for n in [NSWindow.didMoveNotification, NSWindow.didResizeNotification] {
            NotificationCenter.default.addObserver(forName: n, object: window, queue: .main) { [weak self] _ in
                guard let self else { return }
                UserDefaults.standard.set(NSStringFromRect(self.window.frame), forKey: "frame")
            }
        }
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(view)
        buildMenus()

        let mv = UserDefaults.standard.object(forKey: "batteryMV") as? Int ?? 4050
        sim_set_battery(Int32(mv), UserDefaults.standard.object(forKey: "usb") as? Bool ?? true)
        sim_set_tilt(tilt, flat)
        sim_start(CommandLine.argc, CommandLine.unsafeArgv)
        Timer.scheduledTimer(withTimeInterval: 1.0 / 60, repeats: true) { [weak self] _ in self?.view.refresh() }
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { true }

    func turn(_ by: Float) {
        tilt = (tilt + by).truncatingRemainder(dividingBy: 360)
        flat = false
        apply()
    }
    func toggleFlat() { flat.toggle(); apply() }
    private func apply() {
        UserDefaults.standard.set(tilt, forKey: "tilt")
        UserDefaults.standard.set(flat, forKey: "flat")
        sim_set_tilt(tilt, flat)
    }

    // ---- menus
    private func buildMenus() {
        let main = NSMenu()
        let appItem = NSMenuItem(), appMenu = NSMenu()
        appMenu.addItem(withTitle: "Quit \(appName)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        main.addItem(appItem)

        let devItem = NSMenuItem(), dev = NSMenu(title: "Device")
        add(dev, "Turn Left", #selector(turnLeft), "[")
        add(dev, "Turn Right", #selector(turnRight), "]")
        add(dev, "Lay Flat / Stand Up", #selector(flatToggle), "f")
        add(dev, "Pick Up (Shake)", #selector(shake), "k")
        dev.addItem(.separator())
        let batt = NSMenuItem(title: "Battery", action: nil, keyEquivalent: ""), bm = NSMenu()
        for (label, mv) in [("Full (4.15 V)", 4150), ("Half (3.89 V)", 3890), ("Low (3.76 V)", 3760), ("Empty (3.68 V)", 3680)] {
            let i = NSMenuItem(title: label, action: #selector(setBattery(_:)), keyEquivalent: "")
            i.target = self
            i.tag = mv
            bm.addItem(i)
        }
        bm.addItem(.separator())
        let usb = NSMenuItem(title: "On USB (Charging)", action: #selector(toggleUSB(_:)), keyEquivalent: "")
        usb.target = self
        bm.addItem(usb)
        batt.submenu = bm
        dev.addItem(batt)
        dev.addItem(.separator())
        add(dev, "Restart", #selector(restart), "r")
        add(dev, "Erase Flash and Restart…", #selector(erase), "")
        dev.addItem(.separator())
        add(dev, "Show Saved Data in Finder", #selector(reveal), "")
        devItem.submenu = dev
        main.addItem(devItem)

        let winItem = NSMenuItem(), win = NSMenu(title: "Window")
        add(win, "Larger", #selector(larger), "+")
        add(win, "Smaller", #selector(smaller), "-")
        add(win, "Actual Size", #selector(actualSize), "0")
        add(win, "Tiny", #selector(tiny), "9")
        win.addItem(.separator())
        add(win, "Keep on Top", #selector(toggleOnTop(_:)), "")
        add(win, "Close", #selector(NSApplication.terminate(_:)), "w")
        winItem.submenu = win
        main.addItem(winItem)
        NSApp.mainMenu = main
    }

    private func add(_ m: NSMenu, _ t: String, _ s: Selector, _ k: String) {
        let i = NSMenuItem(title: t, action: s, keyEquivalent: k)
        if !k.isEmpty { i.keyEquivalentModifierMask = [.command] }
        i.target = self
        m.addItem(i)
    }

    // Sizes: the glass's 466 pixels at 1x (Actual Size), in steps of a sixth, down to Tiny (a glance-sized corner badge)
    private static let tinySide: CGFloat = 200
    private func resize(_ side: CGFloat) {
        let s = max(App.tinySide, min(1000, side)), f = window.frame
        window.setFrame(NSRect(x: f.midX - s / 2, y: f.midY - s / 2, width: s, height: s), display: true, animate: true)
    }
    @objc private func larger() { resize(window.frame.width * 7 / 6) }
    @objc private func smaller() { resize(window.frame.width * 6 / 7) }
    @objc private func actualSize() { resize(CGFloat(side) + 2 * GlassView.rim) }
    @objc private func tiny() { resize(App.tinySide) }
    @objc private func toggleOnTop(_ i: NSMenuItem) {
        let on = window.level != .floating
        window.level = on ? .floating : .normal
        UserDefaults.standard.set(on, forKey: "onTop")
    }

    @objc private func turnLeft() { turn(-90) }
    @objc private func turnRight() { turn(90) }
    @objc private func flatToggle() { toggleFlat() }
    @objc private func shake() { sim_shake() }
    @objc private func setBattery(_ i: NSMenuItem) {
        UserDefaults.standard.set(i.tag, forKey: "batteryMV")
        sim_set_battery(Int32(i.tag), sim_usb())
    }
    @objc private func toggleUSB(_ i: NSMenuItem) {
        UserDefaults.standard.set(!sim_usb(), forKey: "usb")
        sim_set_battery(sim_battery_mv(), !sim_usb())
    }
    @objc private func restart() { relaunch() }
    @objc private func erase() {
        let a = NSAlert()
        a.messageText = "Erase the simulator's flash?"
        a.informativeText = "Everything it saved goes: Wi-Fi, pairing, calendars, Spotify, calibration. It starts up as if new (already on its pretend Wi-Fi and paired with this Mac)."
        a.addButton(withTitle: "Erase")
        a.addButton(withTitle: "Cancel")
        guard a.runModal() == .alertFirstButtonReturn else { return }
        try? FileManager.default.removeItem(atPath: String(cString: sim_support_dir()) + "/nvs.bin")
        relaunch()
    }
    @objc private func reveal() {
        NSWorkspace.shared.selectFile(String(cString: sim_support_dir()) + "/nvs.bin", inFileViewerRootedAtPath: "")
    }
    private func relaunch() {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: CommandLine.arguments[0])
        p.arguments = Array(CommandLine.arguments.dropFirst())
        try? p.run()
        exit(0)
    }

    @objc func validateMenuItem(_ i: NSMenuItem) -> Bool {
        if i.action == #selector(toggleUSB(_:)) { i.state = sim_usb() ? .on : .off }
        if i.action == #selector(toggleOnTop(_:)) { i.state = window.level == .floating ? .on : .off }
        if i.action == #selector(setBattery(_:)) { i.state = abs(Int(sim_battery_mv()) - i.tag) < 5 ? .on : .off }
        return true
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
app.delegate = App.shared
app.run()
