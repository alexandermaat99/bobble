#!/bin/zsh
# Builds the Reminders helper and runs it in the background at login (launchd agent).
#   ./install.sh            install / update
#   ./install.sh remove     stop it and remove it
set -e
cd "${0:A:h}"
DEST="$HOME/Library/Application Support/AMOLED Calendar"
PLIST="$HOME/Library/LaunchAgents/com.amoled-calendar.reminders-sync.plist"
launchctl bootout gui/$UID "$PLIST" 2>/dev/null || true
if [[ "$1" == "remove" ]]; then rm -f "$PLIST" "$DEST/reminders_sync"; echo "Removed."; exit 0; fi

swiftc -O -swift-version 5 main.swift -o reminders_sync -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker Info.plist 2>/dev/null
mkdir -p "$DEST"
cp reminders_sync "$DEST/"
cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>com.amoled-calendar.reminders-sync</string>
    <key>ProgramArguments</key><array><string>$DEST/reminders_sync</string></array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>ThrottleInterval</key><integer>60</integer>
    <key>StandardOutPath</key><string>$DEST/reminders_sync.log</string>
    <key>StandardErrorPath</key><string>$DEST/reminders_sync.log</string>
</dict>
</plist>
PL
launchctl bootstrap gui/$UID "$PLIST"
echo "Installed. Log: $DEST/reminders_sync.log"
