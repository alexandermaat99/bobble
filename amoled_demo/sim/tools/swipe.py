#!/usr/bin/env python3
# Drags a finger across the simulator's glass: tools/swipe.py x0 y0 x1 y1 [seconds] [port]   (a tap: the same point twice;
# port: Bobble Square's, usually 8467)
import sys, time, urllib.request
x0, y0, x1, y1 = map(int, sys.argv[1:5])
dur = float(sys.argv[5]) if len(sys.argv) > 5 else 0.25
port = sys.argv[6] if len(sys.argv) > 6 else "8466"
def send(q): urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{port}/sim/input?{q}", method="POST"), timeout=3).read()
steps = max(2, int(dur / 0.016))
for i in range(steps + 1):
    t = i / steps
    send(f"touch={round(x0 + (x1 - x0) * t)},{round(y0 + (y1 - y0) * t)}")
    time.sleep(dur / steps)
send("touch=up")
