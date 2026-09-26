#!/usr/bin/env python3
"""Engångsinloggning mot Spotify (Authorization Code med PKCE) för spotify.ino.

Kör på datorn:  python3 spotify_auth.py --client-id DITT_CLIENT_ID

Skriptet öppnar Spotifys inloggning i webbläsaren. Du loggar in och godkänner själv;
lösenordet passerar aldrig det här skriptet. Refresh-token skrivs direkt till secrets.h
(inte till skärmen eller loggen). Kräver bara Pythons standardbibliotek.
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import re
import secrets
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

SCOPES = "user-read-currently-playing user-read-playback-state user-modify-playback-state"
AUTH_URL = "https://accounts.spotify.com/authorize"
TOKEN_URL = "https://accounts.spotify.com/api/token"


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def update_secrets(path: str, client_id: str, refresh_token: str) -> None:
    """Sätter SPOTIFY_CLIENT_ID och SPOTIFY_REFRESH_TOKEN i secrets.h; övriga rader lämnas orörda."""
    lines = []
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    wanted = {"SPOTIFY_CLIENT_ID": client_id, "SPOTIFY_REFRESH_TOKEN": refresh_token}
    done = set()
    out = []
    for ln in lines:
        m = re.match(r"\s*#define\s+(SPOTIFY_CLIENT_ID|SPOTIFY_REFRESH_TOKEN)\b", ln)
        if m:
            key = m.group(1)
            out.append('#define %s "%s"' % (key, wanted[key]))
            done.add(key)
        else:
            out.append(ln)
    for key, val in wanted.items():
        if key not in done:
            out.append('#define %s "%s"' % (key, val))
    tmp = path + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    os.replace(tmp, path)
    os.chmod(path, 0o600)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--client-id", required=True, help="Client ID från din app på developer.spotify.com")
    ap.add_argument("--secrets", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "secrets.h"))
    ap.add_argument("--port", type=int, default=8888)
    ap.add_argument("--no-browser", action="store_true", help="öppna inte webbläsaren automatiskt")
    args = ap.parse_args()

    if not re.fullmatch(r"[0-9a-f]{32}", args.client_id):
        print("Client ID ser inte rätt ut: '%s'\nDet ska vara 32 tecken (siffror och bokstäver a–f), kopierat från din app på"
              " developer.spotify.com under Settings. Skriv INTE texten DITT_CLIENT_ID." % args.client_id)
        return 1

    redirect_uri = "http://127.0.0.1:%d/callback" % args.port
    verifier = b64url(secrets.token_bytes(64))
    challenge = b64url(hashlib.sha256(verifier.encode()).digest())
    state = b64url(secrets.token_bytes(16))
    result = {}
    done = threading.Event()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            q = urllib.parse.urlparse(self.path)
            if q.path != "/callback":
                self.send_response(404); self.end_headers(); return
            params = urllib.parse.parse_qs(q.query)
            if params.get("state", [""])[0] != state:
                result["error"] = "state stämmer inte (avbryter)"
            elif "error" in params:
                result["error"] = params["error"][0]
            else:
                result["code"] = params.get("code", [""])[0]
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            msg = ("<h2>Något gick fel: %s. Se terminalen.</h2>" % result["error"]) if "error" in result \
                else "<h2>Klart! Du kan stänga fliken och gå tillbaka till terminalen.</h2>"
            self.wfile.write(msg.encode())
            done.set()

        def log_message(self, *a):  # tyst
            pass

    try:
        server = http.server.HTTPServer(("127.0.0.1", args.port), Handler)
    except OSError as e:
        print("Kan inte lyssna på 127.0.0.1:%d (%s). Prova --port med ett annat nummer och lägg till samma"
              " redirect-URI i Spotify-appen." % (args.port, e))
        return 1
    threading.Thread(target=server.serve_forever, daemon=True).start()

    url = AUTH_URL + "?" + urllib.parse.urlencode({
        "client_id": args.client_id, "response_type": "code", "redirect_uri": redirect_uri,
        "scope": SCOPES, "code_challenge_method": "S256", "code_challenge": challenge, "state": state,
    })
    print("Öppnar Spotifys inloggning i webbläsaren. Om inget öppnas, klistra in den här adressen:\n")
    print(url + "\n")
    if not args.no_browser:
        webbrowser.open(url)
    print("Väntar på att du loggar in och godkänner ...")
    if not done.wait(timeout=300):
        print("Tidsgräns (5 min). Kör skriptet igen.")
        return 1
    server.shutdown()
    if "error" in result or not result.get("code"):
        print("Inloggningen avbröts eller misslyckades:", result.get("error", "ingen kod"))
        return 1

    body = urllib.parse.urlencode({
        "grant_type": "authorization_code", "code": result["code"], "redirect_uri": redirect_uri,
        "client_id": args.client_id, "code_verifier": verifier,
    }).encode()
    req = urllib.request.Request(TOKEN_URL, data=body, headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            tok = json.load(r)
    except urllib.error.HTTPError as e:
        print("Spotify avvisade kodbytet (%d): %s" % (e.code, e.read().decode(errors="replace")[:300]))
        return 1
    if not tok.get("refresh_token"):
        print("Svaret innehöll ingen refresh token.")
        return 1

    update_secrets(args.secrets, args.client_id, tok["refresh_token"])
    print("Klart! Refresh-token är sparad i %s (visas inte här). Bygg och flasha sedan spotify.ino." % args.secrets)
    return 0


if __name__ == "__main__":
    sys.exit(main())
