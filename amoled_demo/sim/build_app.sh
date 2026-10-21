#!/bin/zsh
# Builds the simulators (the firmware in main/, running on this Mac) into ~/Applications and starts them: "Bobble
# Bubble.app" (the round 1.75" board) and "Bobble Square.app" (the square 4" one, BOARD_SQUARE). They can run together.
#   ./build_app.sh            build, install and (re)start both
#   ./build_app.sh --no-open  just build and install
# Logs: run ~/Applications/Bobble\ Bubble.app/Contents/MacOS/BobbleSim (or Bobble Square's BobbleSquare) from a terminal.
set -e -o pipefail
cd "${0:A:h}"
./link_firmware.sh
swift build -c release 2>&1 | grep -vE "ld: warning: search path" || true
# (the screen's size is fixed at compile time: the square one is its own build)
SIM_DEFINES="BOARD_SQUARE=1 $SIM_DEFINES" swift build -c release --scratch-path .build-square 2>&1 | grep -vE "ld: warning: search path" || true
icons=../../agent_bridge/tools/make_icon.swift            # (the "display" option from Bobble's icon choices)
if [[ -f $icons && ( ! -f build/AppIcon.icns || $icons -nt build/AppIcon.icns ) ]]; then
    set_=build/AppIcon.iconset
    rm -rf $set_; mkdir -p $set_
    swift $icons a build/icon_1024.png 1024
    for s in 16 32 128 256 512; do
        sips -z $s $s build/icon_1024.png --out $set_/icon_${s}x${s}.png >/dev/null
        sips -z $((s * 2)) $((s * 2)) build/icon_1024.png --out $set_/icon_${s}x${s}@2x.png >/dev/null
    done
    iconutil -c icns $set_ -o build/AppIcon.icns
fi

# bundle <name> <bundle id> <executable> <built binary>
bundle() {
    local build="build/$1.app"
    rm -rf "$build"
    mkdir -p "$build/Contents/MacOS" "$build/Contents/Resources"
    cp "$4" "$build/Contents/MacOS/$3"
    [[ -f build/AppIcon.icns ]] && cp build/AppIcon.icns "$build/Contents/Resources/AppIcon.icns"
    cat > "$build/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
    <key>CFBundleName</key><string>$1</string>
    <key>CFBundleDisplayName</key><string>$1</string>
    <key>CFBundleIdentifier</key><string>$2</string>
    <key>CFBundleExecutable</key><string>$3</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSLocalNetworkUsageDescription</key><string>The simulator answers on the local network like a Bobble display, so the Bobble app can reach it.</string>
    <key>NSBonjourServices</key><array><string>_bobble._tcp</string></array>
</dict></plist>
PLIST
    codesign --force --sign - "$build" >/dev/null 2>&1
    pkill -x "$3" 2>/dev/null || true
    mkdir -p ~/Applications
    rm -rf "$HOME/Applications/$1.app"
    cp -R "$build" "$HOME/Applications/$1.app"
    echo "installed ~/Applications/$1.app"
}
# (Bubble keeps the old simulator's id, so its window and settings carry over)
bundle "Bobble Bubble" app.bobble.Simulator BobbleSim .build/release/BobbleSim
bundle "Bobble Square" app.bobble.Square BobbleSquare .build-square/release/BobbleSim
rm -rf "$HOME/Applications/Bobble Simulator.app" "build/Bobble Simulator.app"   # (the old single app)
if [[ $1 != --no-open ]]; then
    sleep 0.5
    open "$HOME/Applications/Bobble Bubble.app"
    open "$HOME/Applications/Bobble Square.app"
fi
