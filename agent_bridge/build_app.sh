#!/bin/zsh
# Builds "Bobble.app" (the Mac app for the Bobble display) into build/ (menu-bar only; signed ad hoc for running on this Mac).
#   ./build_app.sh            build
#   ./build_app.sh --open     build, then (re)start it
#   ./build_app.sh --install  build, copy to /Applications, zip it to ~/Desktop/Bobble.zip, and start it there
#   ./build_app.sh --release  --install, then publish the zip as a GitHub release: every installed copy updates itself
#                             to it within a few hours (needs gh: brew install gh, then gh auth login)
set -e -o pipefail
cd "${0:A:h}"
repo=${BOBBLE_REPO:-$(cat update_repo 2>/dev/null || true)}  # owner/name of the GitHub repo the app updates from (in ./update_repo)
build=$(date +%y%m%d%H%M)                      # the build number: newer is bigger, which is all the updater compares
if [[ $1 == --release ]]; then
    command -v gh >/dev/null || { echo "needs gh: brew install gh, then gh auth login"; exit 1; }
    [[ -n $repo ]] || { echo "put the repo (owner/name) in update_repo"; exit 1; }
fi
swift build -c release 2>&1 | grep -vE "ld: warning: search path" || true
app="build/Bobble.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp .build/release/AgentBridge "$app/Contents/MacOS/AgentBridge"
if [[ ! -f build/AppIcon.icns || tools/make_icon.swift -nt build/AppIcon.icns ]]; then   # the app icon (redrawn when its script changes)
    set=build/AppIcon.iconset
    rm -rf $set; mkdir -p $set
    swift tools/make_icon.swift b build/icon_1024.png 1024
    for s in 16 32 128 256 512; do
        sips -z $s $s build/icon_1024.png --out $set/icon_${s}x${s}.png >/dev/null
        sips -z $((s * 2)) $((s * 2)) build/icon_1024.png --out $set/icon_${s}x${s}@2x.png >/dev/null
    done
    iconutil -c icns $set -o build/AppIcon.icns
fi
cp build/AppIcon.icns "$app/Contents/Resources/AppIcon.icns"
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
    <key>CFBundleName</key><string>Bobble</string>
    <key>CFBundleDisplayName</key><string>Bobble</string>
    <key>CFBundleIdentifier</key><string>app.bobble.Bobble</string>
    <key>CFBundleExecutable</key><string>AgentBridge</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>CFBundleVersion</key><string>$build</string>
    <key>BobbleUpdateRepo</key><string>$repo</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>LSUIElement</key><true/>
    <key>NSLocalNetworkUsageDescription</key><string>Bobble finds your Bobble display on the local network and sends it what your coding agents are doing.</string>
    <key>NSBonjourServices</key><array><string>_bobble._tcp</string></array>
</dict></plist>
PLIST
codesign --force --sign - "$app" >/dev/null 2>&1
echo "built $app"
if [[ $1 == --install || $1 == --release ]]; then                # into /Applications (where Open at login expects it), then start it
    pkill -x AgentBridge 2>/dev/null || true
    rm -rf "/Applications/Agent Bridge.app" "/Applications/Bobble.app"   # (it used to be called Agent Bridge)
    cp -R "$app" /Applications/
    app="/Applications/Bobble.app"
    echo "installed $app"
    rm -f ~/Desktop/Bobble.zip                # (the copy to share: kept in step with what's installed)
    ditto -c -k --keepParent "$app" ~/Desktop/Bobble.zip
    echo "zipped ~/Desktop/Bobble.zip"
fi
if [[ $1 == --release ]]; then                # out to everyone: the newest release is what the apps look at
    gh release create "build-$build" ~/Desktop/Bobble.zip --repo "$repo" --title "Bobble build $build" \
        --notes "${BOBBLE_NOTES:-Bobble build $build}"
    echo "released build $build to $repo"
fi
if [[ $1 == --open || $1 == --install || $1 == --release ]]; then
    pkill -x AgentBridge 2>/dev/null || true
    sleep 0.5
    open "$app"
fi
