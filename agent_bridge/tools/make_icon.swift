// Draws Bobble's app icon: swift tools/make_icon.swift <style a|b|c> <out.png> [size]  (b is the one in use; a and c
// were the other options)
// (build_app.sh turns the 1024 px one into AppIcon.icns)
import AppKit

let args = CommandLine.arguments
let style = args.count > 1 ? args[1] : "b"
let out = args.count > 2 ? args[2] : "icon.png"
let px = args.count > 3 ? Int(args[3])! : 1024

let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8, samplesPerPixel: 4,
                           hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let k = CGFloat(px) / 1024
NSGraphicsContext.current!.cgContext.scaleBy(x: k, y: k)

func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat) -> NSColor { NSColor(srgbRed: r / 255, green: g / 255, blue: b / 255, alpha: 1) }

// macOS icon grid: an 824 px rounded square centred on 1024, with a soft drop shadow
let tile = NSRect(x: 100, y: 100, width: 824, height: 824)
let shape = NSBezierPath(roundedRect: tile, xRadius: 185, yRadius: 185)
NSGraphicsContext.saveGraphicsState()
let sh = NSShadow(); sh.shadowColor = NSColor.black.withAlphaComponent(0.35); sh.shadowBlurRadius = 24; sh.shadowOffset = NSSize(width: 0, height: -10)
sh.set()
rgb(20, 20, 26).setFill(); shape.fill()
NSGraphicsContext.restoreGraphicsState()

func eyes(cy: CGFloat, w: CGFloat, h: CGFloat, gap: CGFloat, color: NSColor) {
    color.setFill()
    for s: CGFloat in [-1, 1] {
        NSBezierPath(ovalIn: NSRect(x: 512 + s * gap - w / 2, y: cy - h / 2, width: w, height: h)).fill()
    }
}

NSGraphicsContext.saveGraphicsState()
shape.addClip()
switch style {
case "a":   // the display itself: a round black glass in a dark bezel, the white eyes resting on it
    NSGradient(starting: rgb(58, 58, 68), ending: rgb(24, 24, 30))!.draw(in: shape, angle: -90)
    let bezel = NSRect(x: 172, y: 172, width: 680, height: 680)
    NSGradient(starting: rgb(70, 70, 80), ending: rgb(30, 30, 36))!.draw(in: NSBezierPath(ovalIn: bezel), angle: -90)
    rgb(0, 0, 0).setFill(); NSBezierPath(ovalIn: bezel.insetBy(dx: 26, dy: 26)).fill()
    eyes(cy: 500, w: 150, h: 136, gap: 118, color: .white)
case "b":   // warm: a coral tile with a white face and dark pill eyes (the menu-bar mark, in colour)
    NSGradient(starting: rgb(255, 138, 101), ending: rgb(226, 86, 72))!.draw(in: shape, angle: -90)
    NSColor.white.setFill(); NSBezierPath(ovalIn: NSRect(x: 232, y: 232, width: 560, height: 560)).fill()
    rgb(28, 28, 34).setFill()
    for s: CGFloat in [-1, 1] {
        NSBezierPath(roundedRect: NSRect(x: 512 + s * 92 - 42, y: 470, width: 84, height: 160), xRadius: 42, yRadius: 42).fill()
    }
default:    // "c": just the eyes on black, with a speech bubble above (what it does: tells you what your agent's up to)
    rgb(8, 8, 10).setFill(); shape.fill()
    let bub = NSBezierPath(roundedRect: NSRect(x: 262, y: 560, width: 500, height: 210), xRadius: 70, yRadius: 70)
    bub.move(to: NSPoint(x: 470, y: 564)); bub.line(to: NSPoint(x: 512, y: 512)); bub.line(to: NSPoint(x: 554, y: 564)); bub.close()
    rgb(206, 238, 216).setFill(); bub.fill()
    rgb(40, 110, 64).setFill()
    for (i, w) in [330, 230].enumerated() {
        NSBezierPath(roundedRect: NSRect(x: 512 - CGFloat(w) / 2, y: 700 - CGFloat(i) * 70, width: CGFloat(w), height: 30), xRadius: 15, yRadius: 15).fill()
    }
    eyes(cy: 340, w: 150, h: 136, gap: 120, color: .white)
}
NSGraphicsContext.restoreGraphicsState()
NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
