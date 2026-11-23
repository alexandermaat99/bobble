#!/usr/bin/env python3
# Pretend to be on track: POSTs made-up lap telemetry to a Bobble's /race 10 times a second, like tools/race_bridge.py
# does from iRacing. For trying the race screen in the simulator (or on a board) without the rig.
#   python3 tools/race_fake.py                    # the simulator (its port file, else localhost:8466)
#   python3 tools/race_fake.py 10.1.10.181        # a board
#   python3 tools/race_fake.py --lap 40           # seconds per lap (default 40, so laps come round quickly)
# The key: BOBBLE_KEY in the environment, else the development token from main/secrets.h.
import json, math, os, random, re, sys, time, urllib.request

args = [a for a in sys.argv[1:] if not a.startswith("--")]
lap_s = float(sys.argv[sys.argv.index("--lap") + 1]) if "--lap" in sys.argv else 40.0
if "--lap" in sys.argv: args.remove(sys.argv[sys.argv.index("--lap") + 1])

def sim_host():
    try:
        port = open(os.path.expanduser("~/Library/Application Support/BobbleSquare/port")).read().strip()
        return f"127.0.0.1:{int(port)}"
    except (OSError, ValueError):
        return "127.0.0.1:8466"

host = args[0] if args else sim_host()
key = os.environ.get("BOBBLE_KEY")
if not key:
    src = open(os.path.join(os.path.dirname(__file__), "..", "main", "secrets.h")).read()
    key = re.search(r'#define\s+OTA_TOKEN\s+"([^"]+)"', src).group(1)
url = f"http://{host}/race"

def post(d):
    req = urllib.request.Request(url, json.dumps(d).encode(), {"Content-Type": "application/json", "X-OTA-Token": key})
    try:
        urllib.request.urlopen(req, timeout=1).read()
    except Exception as e:
        print("post failed:", e)

print(f"on track -> {url} ({lap_s:.0f} s laps). Ctrl-C to stop.")
lap, laps, pos = 1, 12, 7
best, last = 0.0, 0.0
t0 = time.time()
drift, delta, valid = random.uniform(-0.3, 0.3), 0.0, True
while True:
    t = time.time() - t0
    if t >= lap_s:                                    # over the line
        lap_time = 92.0 + delta + random.uniform(-0.05, 0.05)
        if not best: lap_time = 93.2 + random.uniform(-0.4, 0.4)   # (the out lap's slow-ish)
        last = lap_time
        if valid and (not best or lap_time < best): best = lap_time
        lap = lap + 1 if lap < laps else 1
        pos = max(1, min(20, pos + random.choice([-1, 0, 0, 1])))
        t0, t = time.time(), 0
        drift, delta, valid = random.uniform(-0.6, 0.6), 0.0, True
        print(f"lap {lap}: last {last:.3f}, best {best:.3f}")
    # the delta wanders through the lap towards where this lap's heading, with corners adding a little noise
    frac = t / lap_s
    delta += (drift * frac - delta) * 0.08 + 0.02 * math.sin(t * 1.7) * random.random()
    if valid and random.random() < 0.0015: valid = False; print("  off track: lap invalid")
    post({"delta": round(delta, 3), "valid": valid, "lap": lap, "laps": laps, "pos": pos,
          "last": round(last, 3), "best": round(best, 3), "track": "Spa-Francorchamps"})
    time.sleep(0.1)
