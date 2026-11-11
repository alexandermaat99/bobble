#!/usr/bin/env python3
# Saves what's on the simulator's glass as a PNG: tools/glass.py [out.png] [port]
import struct, sys, urllib.request, zlib
out = sys.argv[1] if len(sys.argv) > 1 else "glass.png"
port = sys.argv[2] if len(sys.argv) > 2 else "8466"
d = urllib.request.urlopen(f"http://127.0.0.1:{port}/sim/glass", timeout=5).read()
N = int((len(d) // 2) ** 0.5)               # (480)
rows = []
for y in range(N):
    r = bytearray(b"\0")
    for x in range(N):
        c = struct.unpack_from(">H", d, (y * N + x) * 2)[0]
        r += bytes((((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31))
    rows.append(bytes(r))
def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 2, 0, 0, 0))
                      + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))
print(out)
