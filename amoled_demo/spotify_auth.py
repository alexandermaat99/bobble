#!/usr/bin/env python3
"""One-time Spotify sign-in for Bobble, from the command line (the Bobble app's "Connect Spotify…" does the same).

Opens Spotify's approval page in your browser (PKCE flow, no client secret or password involved),
catches the redirect on http://127.0.0.1:9900/, swaps the code for a refresh token, and hands
that to the board over Wi-Fi (POST /spotify, protected by the same token as OTA updates).

    ./spotify_auth.py                  # board at bobble.local
    ./spotify_auth.py 192.168.68.62    # or by IP
"""
import base64, hashlib, http.server, json, os, re, secrets, sys, threading, urllib.parse, urllib.request, webbrowser

CLIENT_ID = "94a34d8f859244cf9fb932d8b8a7ac74"
REDIRECT = "http://127.0.0.1:9900/"
SCOPES = "user-read-playback-state user-modify-playback-state user-read-currently-playing playlist-read-private playlist-read-collaborative user-library-read user-read-playback-position"
HERE = os.path.dirname(os.path.abspath(__file__))

board = sys.argv[1] if len(sys.argv) > 1 else "bobble.local"
ota_token = re.search(r'OTA_TOKEN "([^"]+)"', open(os.path.join(HERE, "main", "secrets.h")).read()).group(1)

verifier = base64.urlsafe_b64encode(secrets.token_bytes(48)).rstrip(b"=").decode()
challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
state = secrets.token_urlsafe(16)
result = {}


class Callback(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        if q.get("state", [""])[0] != state:
            self.send_error(400, "state mismatch")
            return
        result["code"] = q.get("code", [None])[0]
        result["error"] = q.get("error", [None])[0]
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.end_headers()
        msg = "Approved - you can close this tab." if result["code"] else f"Not approved: {result['error']}"
        self.wfile.write(f"<html><body style='font:20px system-ui;padding:40px'>{msg}</body></html>".encode())
        threading.Thread(target=self.server.shutdown).start()

    def log_message(self, *a):
        pass


server = http.server.HTTPServer(("127.0.0.1", 9900), Callback)
url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
    "client_id": CLIENT_ID, "response_type": "code", "redirect_uri": REDIRECT, "scope": SCOPES,
    "code_challenge_method": "S256", "code_challenge": challenge, "state": state,
})
print("Opening Spotify in your browser - click Agree.\n(If it doesn't open, visit:)\n" + url + "\n")
webbrowser.open(url)
server.serve_forever()

if not result.get("code"):
    sys.exit(f"Spotify didn't approve: {result.get('error')}")

body = urllib.parse.urlencode({
    "grant_type": "authorization_code", "code": result["code"], "redirect_uri": REDIRECT,
    "client_id": CLIENT_ID, "code_verifier": verifier,
}).encode()
req = urllib.request.Request("https://accounts.spotify.com/api/token", body,
                             {"Content-Type": "application/x-www-form-urlencoded"})
tokens = json.load(urllib.request.urlopen(req))
refresh = tokens["refresh_token"]
print("Got Spotify access. Sending it to the board...")

payload = json.dumps({"client_id": CLIENT_ID, "refresh_token": refresh}).encode()
req = urllib.request.Request(f"http://{board}/spotify", payload,
                             {"Content-Type": "application/json", "X-OTA-Token": ota_token})
print("Board says:", urllib.request.urlopen(req, timeout=15).read().decode().strip())
