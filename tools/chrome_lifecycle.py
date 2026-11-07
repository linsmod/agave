#!/usr/bin/env python3
"""Which page-lifecycle events does Chrome actually fire, and in what order?

Serves a probe page plus a destination page over HTTP, drives Chrome over CDP,
and prints every event the probe reported through navigator.sendBeacon (the
report survives the document, so "does unload still fire?" is answered by the
browser rather than by documentation).

Usage:
    python tools/chrome_lifecycle.py [--mode nav|reload|close|tabswitch|none]
                                     [--chrome PATH] [--wait MS]

Modes:
    nav       load the probe, then navigate to /next            (default)
    reload    load the probe, then Page.reload
    close     load the probe, then close the tab
    tabswitch load the probe, open /alt in a new tab, activate it
    none      load the probe only (baseline: what fires on a plain load)

The output is one line per beacon, in arrival order:
    <recv ms>  <event>  <payload>
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

PROBE = """<!doctype html>
<meta charset="utf-8">
<title>lifecycle probe</title>
<body><h1>probe</h1>
<script>
(function () {
  var t0 = performance.now();

  function report(ev, extra) {
    var data = {
      ev: ev,
      t: Math.round(performance.now() - t0),
      vis: document.visibilityState,
      extra: extra || null
    };
    var s = JSON.stringify(data);
    try {
      if (navigator.sendBeacon) navigator.sendBeacon('/log', s);
      else fetch('/log', {method: 'POST', body: s, keepalive: true});
    } catch (e) { }
  }

  report('init', {beacon: typeof navigator.sendBeacon,
                  vis: document.visibilityState,
                  hidden: document.hidden});

  document.addEventListener('visibilitychange', function () {
    report('visibilitychange', {vis: document.visibilityState, hidden: document.hidden});
  });
  window.addEventListener('pagehide', function (e) {
    report('pagehide', {persisted: !!e.persisted});
  });
  window.addEventListener('pageshow', function (e) {
    report('pageshow', {persisted: !!e.persisted});
  });
  window.addEventListener('beforeunload', function () {
    report('beforeunload');
  });
  window.addEventListener('unload', function () {
    report('unload');
  });
  document.addEventListener('freeze', function () { report('freeze'); });
  document.addEventListener('resume', function () { report('resume'); });
})();
</script>
</body>
"""

NEXT = """<!doctype html>
<meta charset="utf-8"><title>next</title><body><h1>next</h1></body>
"""

EVENTS = []
EVENT_LOCK = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        if self.path.startswith("/next"):
            self._send("text/html; charset=utf-8", NEXT.encode())
        elif self.path.startswith("/alt"):
            self._send("text/html; charset=utf-8", NEXT.encode())
        else:
            self._send("text/html; charset=utf-8", PROBE.encode())

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        with EVENT_LOCK:
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


async def page_ws(port, url_contains=None):
    for _ in range(50):
        try:
            tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json", timeout=1))
        except Exception:
            await asyncio.sleep(0.1)
            continue
        pages = [t for t in tabs if t.get("type") == "page"]
        if url_contains:
            pages = [t for t in pages if url_contains in t.get("url", "")]
        if pages:
            return pages[0]["webSocketDebuggerUrl"]
        await asyncio.sleep(0.1)
    raise RuntimeError("no page target found")


def dump_events(start):
    with EVENT_LOCK:
        rows = list(EVENTS)
    for recv, body in rows:
        print("%7.0fms  %s" % ((recv - start) * 1000.0, body))


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="nav",
                    choices=["nav", "reload", "close", "tabswitch", "none"])
    ap.add_argument("--chrome", default=r"C:\Program Files\Google\Chrome\Application\chrome.exe")
    ap.add_argument("--wait", type=int, default=900,
                    help="ms between the actions (load -> action -> settle)")
    ap.add_argument("--settle", type=int, default=1200)
    args = ap.parse_args()

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % port

    cdp_port = 9333
    profile = tempfile.mkdtemp(prefix="agb-lifecycle-")
    chrome = os.path.abspath(args.chrome)
    if not os.path.exists(chrome):
        sys.exit("chrome not found: " + chrome)

    started = time.monotonic()
    proc = await asyncio.create_subprocess_exec(
        chrome, "--headless=new", "--disable-gpu", "--no-first-run",
        "--no-default-browser-check", "--user-data-dir=" + profile,
        "--remote-debugging-port=%d" % cdp_port,
        base + "/probe",
        stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL)

    try:
        ws_url = await page_ws(cdp_port, "probe")
        await asyncio.sleep(args.wait / 1000.0)
        async with websockets.connect(ws_url, max_size=8 * 1024 * 1024) as ws:
            await cdp(ws, "Page.enable")
            if args.mode == "nav":
                await cdp(ws, "Page.navigate", {"url": base + "/next"})
            elif args.mode == "reload":
                await cdp(ws, "Page.reload", {"ignoreCache": False})
            elif args.mode == "close":
                tabs = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % cdp_port))
                target = [t for t in tabs if t.get("type") == "page"][0]["id"]
                await cdp(ws, "Target.closeTarget", {"targetId": target})
            elif args.mode == "tabswitch":
                res = await cdp(ws, "Target.createTarget", {"url": base + "/alt"})
                new_id = res["result"]["targetId"]
                target = None
                tabs = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % cdp_port))
                for t in tabs:
                    if t.get("type") == "page" and "/probe" in t.get("url", ""):
                        target = t["id"]
                if target:
                    await cdp(ws, "Target.activateTarget", {"targetId": target})
                await asyncio.sleep(0.3)
                await cdp(ws, "Target.activateTarget", {"targetId": new_id})
            await asyncio.sleep(args.settle / 1000.0)
    finally:
        try:
            proc.terminate()
        except Exception:
            pass
        server.shutdown()

    print("mode=%s  (events in arrival order)" % args.mode)
    dump_events(started)


if __name__ == "__main__":
    asyncio.run(main())
