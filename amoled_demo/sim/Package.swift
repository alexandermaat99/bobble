// swift-tools-version:5.10
// Bobble Simulator: the display's firmware (main/, unchanged) running on the Mac in a window. Firmware is the C side:
// the firmware's files (linked in by link_firmware.sh) on port/, which plays the board's hardware and ESP-IDF.
import PackageDescription
import Foundation

// Firmware switches to try in the simulator only, e.g. SIM_DEFINES="EYE_COLOR=1" ./build_app.sh
let tryouts: [CSetting] = (ProcessInfo.processInfo.environment["SIM_DEFINES"] ?? "").split(separator: " ").map { d in
    let kv = d.split(separator: "=", maxSplits: 1).map(String.init)
    return .define(kv[0], to: kv.count > 1 ? kv[1] : nil)
}

let package = Package(
    name: "BobbleSim",
    platforms: [.macOS(.v13)],
    targets: [
        .target(name: "Firmware", path: "Sources/Firmware",
                cSettings: [.headerSearchPath("port/include"), .headerSearchPath("port"), .headerSearchPath("fw"),
                            .define("BOBBLE_SIM"),
                            // (the JPEG decoder's callbacks use unsigned int where the Mac's size_t is wider)
                            .unsafeFlags(["-w", "-Wno-error=implicit-function-declaration", "-Wno-incompatible-function-pointer-types"])] + tryouts,
                linkerSettings: [.linkedLibrary("curl")]),
        .executableTarget(name: "BobbleSim", dependencies: ["Firmware"], path: "Sources/BobbleSim"),
    ]
)
