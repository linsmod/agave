#!/usr/bin/env python3
"""Same experiment as chrome_cancel_probe.py, but for agave: when does the old
page's in-flight subresource get cancelled, at navigation start or at commit?

The page is just <link rel=stylesheet href=/slow.css>; the server streams 1 MB
slowly and records when its write fails (= the browser closed the connection).
agave is driven with --nav-after/--nav-url, so the navigation is issued mid
transfer.

    python tools/agave_cancel_probe.py [--nav-after 900]
"""
import argparse
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CHUNK = 16 * 1024
CHUNKS = 64
CHUNK_MS = 50

PAGE = """<!doctype html><meta charset="utf-8"><title>probe</title>
<body><h1>probe</h1>
<link rel="stylesheet" href="/slow.css">
</body>
"""

NEXT = """<!doctype html><meta charset="utf-8"><title>next</title><body><h1>next</h1></body>"""

STATE = {"started": None, "written": 0, "outcome": None, "closed": None}
T0 = None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        if self.path.startswith("/slow.css"):
            self._slow()
        elif self.path.startswith("/next"):
            self._send("text/html; charset=utf-8", NEXT.encode())
        else:
            self._send("text/html; charset=utf-8", PAGE.encode())

    def _slow(self):
        STATE["started"] = time.monotonic()
        self.send_response(200)
        self.send_header("Content-Type", "text/css")
        self.send_header("Content-Length", str(CHUNK * CHUNKS))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        buf = b"/*" + b"x" * (CHUNK - 4) + b"*/\n"
        try:
            for i in range(CHUNKS):
                self.wfile.write(buf[:CHUNK])
                self.wfile.flush()
                STATE["written"] = (i + 1) * CHUNK
                time.sleep(CHUNK_MS / 1000.0)
            STATE["outcome"] = "completed on the wire"
        except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError) as e:
            STATE["closed"] = time.monotonic()
            STATE["outcome"] = "browser closed after %d bytes (%s)" % (
                STATE["written"], type(e).__name__)

    def _send(self, mime, body):
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nav-after", type=int, default=900)
    ap.add_argument("--exe", default="build-zig-native/agave_shell.exe")
    args = ap.parse_args()

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % port

    global T0
    T0 = time.monotonic()
    proc = subprocess.run(
        [args.exe, "--probe", base + "/probe",
         "--nav-after", str(args.nav_after), "--nav-url", base + "/next",
         "--wait", "2500", "--settle", "200", "--timeout", "15000",
         "--eval", "probeExit(document.title)"],
        capture_output=True, text=True, timeout=60)
    server.shutdown()

    print("agave exit=%d  nav-after=%d ms" % (proc.returncode, args.nav_after))
    for line in proc.stderr.splitlines():
        if "[nav] open url" in line or "watchdog" in line or "[exit]" in line:
            print("  " + line.strip())
    started = STATE["started"]
    closed = STATE["closed"]
    print("css: started %.0fms, closed %s, %d bytes, %s" % (
        (started - T0) * 1000.0,
        ("%.0fms" % ((closed - T0) * 1000.0)) if closed else "-",
        STATE["written"], STATE["outcome"]))


if __name__ == "__main__":
    sys.exit(main())
