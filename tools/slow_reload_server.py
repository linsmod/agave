#!/usr/bin/env python3
"""Slow-page server for the "reload while loading" crash probes.

Serves a document whose subresources finish *after* a reload has started, plus
a sticky header so the layer tree has more than the plain document layer.

    python tools/slow_reload_server.py [port]      # default 8977

Paths:
    /                   the page (a slow <img>, a slow <link>, a sticky header)
    /slow.png?ms=1200   1x1 PNG after `ms` milliseconds
    /slow.css?ms=800    stylesheet after `ms` milliseconds

Usage with the probe (reload every 400 ms, three times):

    agave_shell.exe --probe http://127.0.0.1:8977/ \
        --reload-every 400 --reload-count 3 --wait 3000 \
        --eval "probeExit('rs=' + document.readyState)"
"""
import base64
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAoAAAAKCAIAAAACUFjqAAAAEklEQVR4nGP8z4APMOGVHbHSAEEsAROxCnMTAAAAAElFTkSuQmCC"
)

PAGE = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>slow reload</title>
<link rel="stylesheet" href="/slow.css?ms=800">
<style>
  html, body { margin: 0; padding: 0; }
  #head { position: sticky; top: 0; height: 40px; background: #cc0000; color: #fff; }
  #spacer { height: 2400px; background: #eeeeee; }
  #foot { position: sticky; bottom: 0; height: 30px; background: #0000cc; }
</style></head>
<body>
<div id="head">HEAD</div>
<img id="pic" src="/slow.png?ms=1500" width="120" height="120" alt="">
<div id="body">%s</div>
<div id="spacer"></div>
<div id="foot">FOOT</div>
</body></html>
"""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        delay_ms = int(query.get("ms", ["0"])[0])

        if parsed.path == "/":
            body = (PAGE % ("lorem ipsum " * 400)).encode()
            self._send("text/html; charset=utf-8", body)
        elif parsed.path == "/slow.png":
            time.sleep(delay_ms / 1000.0)
            self._send("image/png", PNG)
        elif parsed.path == "/slow.css":
            time.sleep(delay_ms / 1000.0)
            self._send("text/css", b"#body { color: rgb(1, 2, 3); }\n")
        else:
            body = b"<html><body><h1>other</h1></body></html>"
            self._send("text/html; charset=utf-8", body)

    def _send(self, mime, body):
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        sys.stderr.write("[slow] %s\n" % (fmt % args))


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8977
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print("slow reload server on http://127.0.0.1:%d/" % port, flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
