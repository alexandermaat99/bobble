#!/usr/bin/env python3
# iRacing -> Bobble: runs on the rig PC (Windows, next to iRacing), reads the sim's telemetry and POSTs it to the
# Bobble's /race 10 times a second while you're in the car. The Bobble shows your lap delta and goes back to its usual
# pages a few seconds after you get out.
#   pip install pyirsdk
#   set BOBBLE_KEY=<the Bobble's key>          (the development token from main/secrets.h, or a paired Mac's key)
#   python race_bridge.py 10.1.10.181          (the Bobble's IP; bobble.local works too, but slowly)
import http.client, json, os, sys, time

try:
    import irsdk
except ImportError:
    sys.exit("needs pyirsdk: pip install pyirsdk")

if len(sys.argv) < 2: sys.exit("usage: race_bridge.py <bobble ip>")
host, key = sys.argv[1], os.environ.get("BOBBLE_KEY")
if not key: sys.exit("set BOBBLE_KEY to the Bobble's key first")
HZ = 10

conn = None
def post(d):
    global conn                                       # (one kept-alive connection: a new one per post is slow)
    body = json.dumps(d)
    for attempt in (1, 2):
        try:
            if conn is None: conn = http.client.HTTPConnection(host, timeout=2)
            conn.request("POST", "/race", body, {"Content-Type": "application/json", "X-OTA-Token": key})
            r = conn.getresponse(); r.read()
            if r.status == 403: sys.exit("the Bobble said bad token: check BOBBLE_KEY")
            return
        except (OSError, http.client.HTTPException):
            conn = None
            if attempt == 2: print("can't reach the Bobble at", host)

def session_laps(ir):
    try:
        s = ir["SessionInfo"]["Sessions"][ir["SessionNum"]]["SessionLaps"]
        return int(s)                                 # ("unlimited" in practice: no total)
    except (TypeError, KeyError, IndexError, ValueError):
        return 0

def track(ir):
    try: return ir["WeekendInfo"]["TrackDisplayName"]
    except (TypeError, KeyError): return ""

ir, connected, on_track = irsdk.IRSDK(), False, False
print(f"waiting for iRacing (posting to {host})...")
while True:
    if not connected:
        connected = ir.startup()
        if connected: print("connected to iRacing:", track(ir))
        else: time.sleep(1); continue
    if not (ir.is_initialized and ir.is_connected):
        ir.shutdown(); connected = False; print("iRacing closed"); continue
    ir.freeze_var_buffer_latest()
    if not ir["IsOnTrack"]:                           # in the garage / replay: stop posting (the Bobble goes back)
        if on_track: print("out of the car"); on_track = False
        time.sleep(0.5); continue
    if not on_track: print("in the car"); on_track = True
    pos = ir["PlayerCarClassPosition"] or ir["PlayerCarPosition"] or 0
    post({
        "delta": round(ir["LapDeltaToBestLap"] or 0, 3),
        "valid": bool(ir["LapDeltaToBestLap_OK"]),
        "lap": ir["Lap"] or 0,
        "laps": session_laps(ir),
        "pos": pos,
        "last": round(max(ir["LapLastLapTime"] or 0, 0), 3),
        "best": round(max(ir["LapBestLapTime"] or 0, 0), 3),
        "track": track(ir),
    })
    time.sleep(1 / HZ)
