#!/usr/bin/env python3
"""Does Chrome abort the old page's in-flight fetch when a navigation *starts*,
or only when the new document commits?

The probe page streams /slow (1 MB, 16 KB every 50 ms) and beacons every chunk;
a CDP Page.navigate to /next is issued while it runs, and /next delays its
*headers* by 2 s, so "navigation started" and "document committed" are 2 s
apart. The server also records when its write to /slow fails (= the browser
closed the connection) and after how many bytes.

Usage: python tools/chrome_cancel_probe.py [--chrome PATH] [--commit-delay MS]
Output: one line per beacon plus the /slow transfer outcome.
"""
import argparse
import asyncio
import json
import os
import sys
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import websockets
except ImportError:
    sys.exit("missing 'websockets' (pip install websockets)")

CHUNK = 16 * 1024
CHUNKS = 64            # 1 MB total
CHUNK_MS = 50          # ~3.2 s of streaming

PROBE = """<!doctype html>
<meta charset="utf-8">
<title>cancel probe</title>
<body><h1>cancel probe</h1>
<script>
(function () {
  var t0 = performance.now();
  function log(s) {
    try { navigator.sendBeacon('/log', s + ' @' + Math.round(performance.now() - t0)); } catch (e) {}
  }
  log('fetch-start');
  fetch('/slow').then(function (r) {
    log('headers status=' + r.status);
    var reader = r.body.getReader();
    var total = 0;
    function pump() {
      return reader.read().then(function (res) {
        if (res.done) { log('fetch-DONE bytes=' + total); return; }
        total += res.value.length;
        log('chunk bytes=' + total);
        return pump();
      });
    }
    return pump();
  }).catch(function (e) {
    log('fetch-ERROR ' + e.name + ' bytes=' + (window.__bytes || 0));
  });
})();
</script>
<!-- After the inline script so its fetch starts first; the stylesheet is the
     subresource half of the experiment (a parser-blocking link placed before
     the script would delay the fetch until the CSS finished). -->
<link rel="stylesheet" href="/slow.css">
</body>
"""

NEXT = """<!doctype html>
<meta charset="utf-8"><title>next</title><body><h1>next</h1></body>
"""

EVENTS = []
LOCK = threading.Lock()
STATE = {"slow_started": None, "slow_written": 0, "slow_outcome": None,
         "css_started": None, "css_written": 0, "css_outcome": None}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        if self.path.startswith("/slow.css"):
            self._slow("text/css", "css")
        elif self.path.startswith("/slow"):
            self._slow("application/octet-stream", "slow")
        elif self.path.startswith("/next"):
            time.sleep(COMMIT_DELAY / 1000.0)
            self._send("text/html; charset=utf-8", NEXT.encode())
        else:
            self._send("text/html; charset=utf-8", PROBE.encode())

    def _slow(self, mime, key):
        STATE[key + "_started"] = time.monotonic()
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(CHUNK * CHUNKS))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        buf = b"x" * CHUNK
        try:
            for i in range(CHUNKS):
                self.wfile.write(buf)
                self.wfile.flush()
                STATE[key + "_written"] = (i + 1) * CHUNK
                time.sleep(CHUNK_MS / 1000.0)
            STATE[key + "_outcome"] = "completed on the wire"
            self._send_marker(key + "-complete")
        except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError) as e:
            STATE[key + "_outcome"] = "browser closed the connection after %d bytes (%s)" % (
                STATE[key + "_written"], type(e).__name__)
            self._send_marker(key + "-aborted")

    def _send_marker(self, what):
        with LOCK:
            EVENTS.append((time.monotonic(), "SERVER " + what))

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        with LOCK:
            EVENTS.append((time.monotonic(), body))
        self._send("text/plain", b"ok")

    def _send(self, mime, body):
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


async def cdp(ws, method, params=None, mid=[0]):
    mid[0] += 1
    msg_id = mid[0]
    await ws.send(json.dumps({"id": msg_id, "method": method, "params": params or {}}))
    while True:
        msg = json.loads(await ws.recv())
        if msg.get("id") == msg_id:
            return msg


async def page_ws(port):
    for _ in range(60):
        try:
            tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json", timeout=1))
        except Exception:
            await asyncio.sleep(0.1)
            continue
        pages = [t for t in tabs if t.get("type") == "page"]
        if pages:
            return pages[0]["webSocketDebuggerUrl"]
        await asyncio.sleep(0.1)
    raise RuntimeError("no page target")


async def main():
    global COMMIT_DELAY
    ap = argparse.ArgumentParser()
    ap.add_argument("--chrome", default=r"C:\Program Files\Google\Chrome\Application\chrome.exe")
    ap.add_argument("--commit-delay", type=int, default=2000,
                    help="ms /next waits before sending its headers (delays commit)")
    ap.add_argument("--start-after", type=int, default=400,
                    help="ms after the probe loads that the navigation is issued")
    ap.add_argument("--settle", type=int, default=6000)
    args = ap.parse_args()
    COMMIT_DELAY = args.commit_delay

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % port

    cdp_port = 9444
    profile = tempfile.mkdtemp(prefix="agb-cancel-")
    chrome = os.path.abspath(args.chrome)
    if not os.path.exists(chrome):
        sys.exit("chrome not found: " + chrome)

    started = time.monotonic()
    proc = await asyncio.create_subprocess_exec(
        chrome, "--headless=new", "--disable-gpu", "--no-first-run",
        "--no-default-browser-check", "--user-data-dir=" + profile,
        "--remote-debugging-port=%d" % cdp_port, base + "/probe",
        stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL)

    nav_at = None
    try:
        ws_url = await page_ws(cdp_port)
        await asyncio.sleep(args.start_after / 1000.0)
        async with websockets.connect(ws_url, max_size=8 * 1024 * 1024) as ws:
            await cdp(ws, "Page.enable")
            nav_at = time.monotonic()
            with LOCK:
                EVENTS.append((nav_at, "CLIENT Page.navigate -> /next (commit delayed %d ms)" % COMMIT_DELAY))
            await cdp(ws, "Page.navigate", {"url": base + "/next"})
            await asyncio.sleep(args.settle / 1000.0)
    finally:
        try:
            proc.terminate()
        except Exception:
            pass
        server.shutdown()

    print("commit delay=%d ms, navigation issued at %.0f ms" % (COMMIT_DELAY, (nav_at - started) * 1000.0))
    with LOCK:
        rows = list(EVENTS)
    for at, what in rows:
        print("%8.0fms  %s" % ((at - started) * 1000.0, what))
    print("slow outcome: %s (%d bytes written)" % (STATE["slow_outcome"], STATE["slow_written"]))
    print("css  outcome: %s (%d bytes written)" % (STATE["css_outcome"], STATE["css_written"]))


COMMIT_DELAY = 2000

if __name__ == "__main__":
    asyncio.run(main())
