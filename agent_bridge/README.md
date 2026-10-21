# Bobble (Mac app)

The Mac side of the Bobble display (the code still lives in `agent_bridge/`): a menu-bar app that takes Claude Code's hook events, works out what each chat is
doing, and relays it to the display on the local network.

    Claude Code hook ──POST /hook───┐
    Codex hook ───────POST /codex───┤
    Cursor hook ──────POST /cursor──┴▶ Bobble.app (127.0.0.1:47823) ──POST /agent──▶ display (Bonjour _bobble._tcp, bobble.local)

- `HookServer.swift`: receives hook events on this Mac only, answers at once.
- `Chats.swift`: each chat's state, message, model, title and integration (ported from the prototype's `agent_hook.sh`).
- `Device.swift`: finds the display (`_bobble._tcp`), sends the newest state of each chat (retrying while it's away), pairs, and reads/sets its calendars (`GET`/`POST /calendars`, the private .ics links, kept on the display).
- `Hooks.swift`: adds or removes the one-line hook in `~/.claude/settings.json`, Codex's in `~/.codex/hooks.json` (same
  format; Codex runs it only after you trust it with `/hooks`), and Cursor's in `~/.cursor/hooks.json`
  (backups kept next to them). Cursor only gets hooks that can't block anything (its "before" hooks treat a missing
  answer as deny), so Cursor chats don't show "needs you" yet.
- `Spotify.swift`: "Connect Spotify…": Spotify's approval page in the browser (PKCE, no secret), the redirect caught on
  http://127.0.0.1:9900/ while signing in, the code swapped for a refresh token, handed to the display (`POST /spotify`;
  `GET` says whether it's connected, `DELETE` disconnects). The display then talks to Spotify itself.
- `main.swift`: the menu (display status, chats, privacy switch, connect/disconnect, pairing, calendars, Spotify, open at login).

Pairing: "Pair with display…" asks the display for a code (`POST /pair/start`; it shows 6 digits for 2 minutes),
you type it in, and `POST /pair/finish` returns a key of this Mac's own, kept in ~/Library/Application Support/Bobble/display-key (mode 600; back to the Keychain once the app is signed properly: ad-hoc builds make macOS re-ask for the Keychain password). Five wrong codes cancel
it; pairing again replaces the key. (The firmware's development token still works too, as a fallback.)

Build and run: `./build_app.sh --open` (makes `build/Bobble.app`), or `--install` to put it in /Applications.

Updates: `./build_app.sh --release` installs, then publishes `Bobble.zip` as a release (tag `build-<yymmddHHMM>`) of the
repo in `update_repo` (alexandermaat99/bobble, public; needs `gh auth login` once). `Updater.swift` checks the latest
release 20 s after launch, every 4 hours, and from "Check for Updates…". A higher build number gets downloaded,
checked (bundle id, build, codesign --verify), swapped in for the copy in /Applications, and started again. Copies
outside an Applications folder (build/) don't update themselves.

A release (`./build_app.sh --release "what changed"`) is built from a clean copy of what's committed, named "Bobble
<VERSION> — <summary>", with notes from the commits since the last one. It carries the display's release firmware
and the pieces a blank board needs (bootloader, partition table, otadata). The app sends the firmware to its paired
board once no chat is busy. `publish_pages.sh` then puts the setup page (`amoled_demo/flasher/index.html`, ESP Web
Tools) and those pieces on the gh-pages branch, for flashing new Bobbles from the browser.

Next: a persistent connection, approving permission prompts from the display, and adapters for other agents.
