#!/bin/zsh
# Builds "Bobble.app" (the Mac app for the Bobble display) into build/ (menu-bar only; signed ad hoc for running on this Mac).
#   ./build_app.sh            build
#   ./build_app.sh --open     build, then (re)start it
#   ./build_app.sh --install  build, copy to /Applications, zip it to ~/Desktop/Bobble.zip, and start it there
#   ./build_app.sh --release "Pairing code can be dismissed"
#                             the next version (0.3, 0.4… in ./VERSION), --install, and the display's release firmware,
#                             then publish both as a GitHub release ("Bobble 0.3 — <summary>", notes from the commits
#                             since the last one): every installed copy updates itself, then its paired Bobble, within a
#                             few hours (needs gh: brew install gh, then gh auth login)
set -e -o pipefail
cd "${0:A:h}"
repo=${BOBBLE_REPO:-$(cat update_repo 2>/dev/null || true)}  # owner/name of the GitHub repo the app updates from (in ./update_repo)
build=$(date +%y%m%d%H%M)                      # the build number: newer is bigger, which is all the updater compares
src=${BOBBLE_SNAPSHOT:-$PWD}                   # (the real agent_bridge, for git and VERSION, when building a release copy)
version=$(cat $src/VERSION 2>/dev/null || echo 0.1)   # the version people see (the last one released)
if [[ $1 == --release && -z $BOBBLE_SNAPSHOT ]]; then
    # A release is what's committed: built in a clean copy of HEAD, so work in progress (and the development token)
    # stays out of it
    command -v gh >/dev/null || { echo "needs gh: brew install gh, then gh auth login"; exit 1; }
    [[ -n $repo ]] || { echo "put the repo (owner/name) in update_repo"; exit 1; }
    [[ -n $2 ]] || { echo 'say what changed: ./build_app.sh --release "Pairing code can be dismissed"'; exit 1; }
    [[ -z $(git status --porcelain -- . ../amoled_demo) ]] || echo "note: uncommitted changes are left out (a release is what's committed)"
    snap=$(mktemp -d)/bobble
    mkdir -p $snap
    git -C .. archive HEAD agent_bridge amoled_demo | tar -x -C $snap
    cp -R ../amoled_demo/managed_components $snap/amoled_demo/ 2>/dev/null || true   # (saves downloading them again)
    cp $snap/amoled_demo/main/secrets.h.example $snap/amoled_demo/main/secrets.h       # (a release takes no development token)
    mkdir -p $snap/agent_bridge/build; cp build/AppIcon.icns $snap/agent_bridge/build/ 2>/dev/null || true
    BOBBLE_SNAPSHOT=$PWD exec $snap/agent_bridge/build_app.sh --release "$2"
fi
if [[ $1 == --release ]]; then                # (in the clean copy now)
    summary=$2
    version=${version%.*}.$(( ${version##*.} + 1 ))   # 0.2 -> 0.3
    echo "building the display firmware (release, build $build)..."
    fw=../amoled_demo
    if ! (cd $fw && . ~/esp/esp-idf/export.sh >/dev/null 2>&1 && mkdir -p build-release &&
          idf.py -B build-release -D BOBBLE_RELEASE=1 -D BOBBLE_BUILD=$build build > build-release/last_build.log 2>&1); then
        grep -E "error:" $fw/build-release/last_build.log | head -20
        echo "firmware build failed - nothing released (full output: $fw/build-release/last_build.log)"
        exit 1
    fi
    mkdir -p build
    cp $fw/build-release/amoled_demo.bin build/bobble-firmware.bin
    # (and what a blank board needs besides, for flash_new.sh and the setup page: offsets as in flash_args)
    cp $fw/build-release/bootloader/bootloader.bin build/bobble-bootloader.bin
    cp $fw/build-release/partition_table/partition-table.bin build/bobble-partitions.bin
    cp $fw/build-release/ota_data_initial.bin build/bobble-otadata.bin
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
    <key>CFBundleShortVersionString</key><string>$version</string>
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
    git -C $src fetch -q --tags origin
    prev=$(git -C $src tag -l 'build-*' | sort | tail -1)
    log() { [[ -n $prev ]] && git -C $src log --format='- %s' "$prev..HEAD" -- "$@" | grep -v "^- Bobble [0-9.]*$" || true }
    app_log=$(log . ':!VERSION'); fw_log=$(log ../amoled_demo)
    echo $version > $src/VERSION               # (committed and pushed, so the release's tag has it)
    git -C $src commit -q -m "Bobble $version" -- VERSION
    git -C $src push -q
    notes="$summary"
    [[ -n $app_log ]] && notes+=$'\n\n**Mac app**\n'"$app_log"
    [[ -n $fw_log ]] && notes+=$'\n\n**Display**\n'"$fw_log"
    notes+=$'\n\n'"Build $build. The Mac app updates itself, then its paired Bobble once no chat is busy."
    gh release create "build-$build" ~/Desktop/Bobble.zip build/bobble-{firmware,bootloader,partitions,otadata}.bin --repo "$repo" \
        --target "$(git -C $src rev-parse HEAD)" --title "Bobble $version — $summary" --notes "$notes"
    echo "released Bobble $version (build $build) to $repo"
    ./publish_pages.sh build $version            # (the setup page, for flashing new Bobbles from the browser)
fi
if [[ $1 == --open || $1 == --install || $1 == --release ]]; then
    pkill -x AgentBridge 2>/dev/null || true
    sleep 0.5
    open "$app"
fi
