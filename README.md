# Bobble

A desk display that shows what your coding agents are doing: each chat's progress, what it's waiting on, and buttons to
answer its questions. It also shows your calendar and what's playing on Spotify. Bobble Square is the display; Bobble is
the Mac menu-bar app that sends it your Claude Code, Codex and Cursor activity.

## Set up your Bobble

**Fastest:** paste this repo's link into Claude Code and say *"set up my Bobble"*. It follows the steps below and tells
you when it needs you to do something on the display or in the menu bar.

You need a Mac on macOS 13 or later, and Wi-Fi the Bobble can join (2.4 GHz; most home networks have it).

1. **Get the Bobble online.** Power it on. On its screen, tap the robot's speech bubble, then scan the QR code with your
   phone and pick your Wi-Fi. (Or pull down from the top edge of the screen, tap Wi-Fi, choose your network and type the
   password.)
2. **Install the Mac app.** Download
   [Bobble.zip](https://github.com/alexandermaat99/bobble/releases/latest/download/Bobble.zip), unzip it, and move
   Bobble.app to Applications. Open it; a small face appears in the menu bar. (If you downloaded it in a browser, macOS
   blocks the first launch: open System Settings › Privacy & Security and click Open Anyway.)
3. **Pair.** With the Mac on the same Wi-Fi as the Bobble, click the face in the menu bar and choose **Pair with
   Display…**. Type the 6-digit code that appears on the Bobble.
4. **Show your chats.** In the same menu, under **Show Chats From**, choose **Claude Code** (and Codex or Cursor if you
   use them). Chats you start from then on appear on the Bobble.
5. **Optional:** **Calendars…** to show your calendars, and **Connect Spotify…** for what's playing.

After that, the app keeps itself and the Bobble up to date.

### For Claude Code (or another agent) doing this setup

Work through the steps with the user. Do what you can from the terminal, and ask them for the rest: they have to touch the
display (Wi-Fi) and click in the menu bar (pairing, choosing Claude Code).

- **Install the app** (a download with curl isn't quarantined, so macOS doesn't block the first launch):
  ```sh
  curl -fL -o /tmp/Bobble.zip https://github.com/alexandermaat99/bobble/releases/latest/download/Bobble.zip
  ditto -x -k /tmp/Bobble.zip /Applications && rm /tmp/Bobble.zip
  open /Applications/Bobble.app
  ```
  If `/Applications/Bobble.app` already exists, it updates itself; just `open` it.
- **Is the Bobble on the network?** `dns-sd -B _bobble._tcp` (stop it after a few seconds) should list it, for example
  "Bobble 3F2A". If nothing shows up, it isn't on Wi-Fi yet (step 1), or the Mac is on a different network.
- **Pairing and Claude Code:** ask the user to do steps 3 and 4 in the menu bar. Don't edit hook settings yourself:
  the app writes them and keeps a backup. To check them afterwards, `grep -c AGENT_BRIDGE=1 ~/.claude/settings.json`
  should be above 0. Hooks apply to Claude Code sessions started after that, so the current one won't show on the
  display.
- **Done when** the menu shows the Bobble's name with a green dot and "Connected", and a new Claude Code session appears on the display.

### Troubleshooting

- **The menu says "Looking for a display…":** the Mac and the Bobble must be on the same network. Guest and office networks
  sometimes keep devices apart.
- **"Needs pairing":** choose Pair with Display… again. A code is good for 2 minutes.
- **A blank or broken Bobble:** with it plugged into the Mac over USB, open the
  [setup page](https://alexandermaat99.github.io/bobble/) in Chrome or Edge and click Install. That puts the latest
  release on it and erases its settings. Or run `amoled_demo/flash_new.sh` (needs `python3 -m pip install --user esptool`).

## Developing

- `amoled_demo/`: the display's firmware for Bobble Square (ESP-IDF 5.5, the Waveshare ESP32-S3-Touch-LCD-4B), and
  `sim/`, the same firmware running on the Mac as the Bobble Square simulator. Before building, copy
  `main/secrets.h.example` to `main/secrets.h`. Build with `idf.py build` (or `./ota.sh` to build and send it to the
  board over Wi-Fi); a release build (only paired Macs get in) is `idf.py -B build-release -D BOBBLE_RELEASE=1 build`.
  The round Bobble Bubble (the 1.75" AMOLED board) is archived: its last state is the `bubble-archive` tag.
- `agent_bridge/`: Bobble.app, the menu-bar app that takes Claude Code, Codex and Cursor hook events and sends them to
  the display. `./build_app.sh --release` publishes a new build; installed copies update themselves.
