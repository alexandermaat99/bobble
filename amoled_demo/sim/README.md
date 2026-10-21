# Bobble Bubble and Bobble Square (the simulators)

The firmware in `main/`, unchanged, running on the Mac in a window. Only the hardware underneath is swapped: `port/`
plays the panel, touch, motion sensor, BOOT button, battery, flash (NVS in a file), the web server (a real socket on
port 8466) and the HTTP client (libcurl). `port/sim_wifi.c` replaces `wifi_setup.c` (pretend networks; the Mac's own
connection does the work). Everything else, `main.c` included, is the firmware's own code.

Two apps, which run side by side: **Bobble Bubble** (the round 1.75" AMOLED, 466x466) and **Bobble Square** (the 4"
square LCD board, 480x480: the firmware built with `BOARD_SQUARE`). Each has its own flash and port (the second one
running takes the next free port), and the Bobble app sends chats to both.

    ./build_app.sh        # links main/ in, builds both, installs them in ~/Applications and opens them

Use it like the board: click and drag on the glass. Keys: **B** (hold) is BOOT, **[** and **]** turn it, **F** lays it flat.
The Device menu also picks it up, sets the battery and USB, restarts it and erases its flash.

The Bobble app finds each one by the port file it leaves in its data folder, sends it every chat as well as the board,
and has a section for each with its pairing, calendars and Spotify. On first launch it's already on its pretend
Wi-Fi and paired with this Mac's Bobble app. A factory reset (Settings) or Device › Erase Flash gives the out-of-the-box setup.

For scripts and previews:

    tools/glass.py out.png [port]          # what's on the glass, as a PNG (Square: its port, usually 8467)
    tools/swipe.py 400 233 60 233          # drag a finger (the same point twice: a tap)
    curl -X POST 'localhost:8466/sim/input?boot=1'    # also tilt=<degrees>, flat=1, touch=x,y / touch=up

Logs go to stdout: run `~/Applications/Bobble\ Bubble.app/Contents/MacOS/BobbleSim` (or Bobble Square's `BobbleSquare`)
from a terminal, or read `/log` as for the board. Saved data: `~/Library/Application Support/BobbleSim/nvs.bin` (Bubble)
and `.../BobbleSquare/nvs.bin` (Square), each next to its `port` file.
