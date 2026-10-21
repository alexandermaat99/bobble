# Bobble

A desk display that shows what your coding agents are doing, with its Mac app.

- `amoled_demo/`: the display's firmware (ESP-IDF 5.5, ESP32-S3 AMOLED board), and `sim/`, the same firmware running on
  the Mac as the Bobble Simulator. Before building, copy `main/secrets.h.example` to `main/secrets.h`.
  Build with `idf.py build`; a release build (only paired Macs get in) is `idf.py -B build-release -D BOBBLE_RELEASE=1 build`.
- `agent_bridge/`: Bobble.app, the menu-bar app that takes Claude Code, Codex and Cursor hook events and sends them to
  the display. `./build_app.sh --release` publishes a new build; installed copies update themselves.

**A new Bobble:** open the setup page, https://alexandermaat99.github.io/bobble/, in Chrome or Edge with the board
plugged in over USB, and click Install. Or run `amoled_demo/flash_new.sh`, which needs only `esptool`
(`python3 -m pip install --user esptool`). Either way it gets the latest release; then tap the bubble on its screen to
set up Wi-Fi, and pair it from the Mac app.

Download the app from [Releases](../../releases/latest).
