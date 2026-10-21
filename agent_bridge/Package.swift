// swift-tools-version:5.10
// Agent Bridge: the Mac side of the agent display. A menu-bar app that takes Claude Code's hook events, works out
// what each chat is doing, and relays it to the display on the local network (and to the Bobble Simulator, when it's
// running: ~/esp/amoled_demo/sim).
import PackageDescription

let package = Package(
    name: "AgentBridge",
    platforms: [.macOS(.v13)],
    targets: [.executableTarget(name: "AgentBridge", path: "Sources/AgentBridge")]
)
