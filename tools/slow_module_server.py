#!/usr/bin/env python3
"""Streamed HTML + slow entry module, for the tokenizer "data while the parser
is blocked on a routed entry module" case (the v.qq.com crash).

Why the padding: the native backend reads with WinHttpReadData into 16 KiB
blocks and only sees data once a block fills (or the body ends). The first
chunk therefore carries >16 KiB before the module tag, so the tokenizer keeps
getting writes while the entry module is still downloading.

Layout of the response:
  chunk 1 (immediately): ~20 KiB of padding + `<script type="module" src=...>`
                         + `</script>` + ~20 KiB of padding
  chunk 2 (0.5 s later): `<script>window.__secondScript = 1;</script>` + tail
The module body itself is delayed 1.5 s, so chunk 2 lands while the parser is
blocked waiting for it. With the bug the tokenizer parses chunk 2 anyway, sees
a script start tag while the held module element is still in `scriptNode`, and
`ASSERT(!scriptNode)` -> CRASH() (exit 139).

Usage: python tools/slow_module_server.py [port]
Then:  agave_shell.exe --probe http://127.0.0.1:8974/ --wait 5000
"""

import http.server
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8974

PAD = 20 * 1024

HEAD = (b"<!doctype html>\n<html><head><meta charset=\"utf-8\">"
        b"<title>slow module</title>\n<!-- " + b"p" * PAD + b" -->\n"
        b"<script type=\"module\" src=\"/slow-module.js\"></script>\n"
        b"<!-- " + b"q" * PAD + b" -->\n")
TAIL = b"""<script>window.__secondScript = 1;</script>
</head><body><p id="ok">after module</p></body></html>
"""

MODULE = b"window.__moduleRan = 1;\n"


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        if self.path.startswith("/slow-module.js"):
            time.sleep(1.5)
            self.send_response(200)
            self.send_header("Content-Type", "text/javascript; charset=utf-8")
            self.send_header("Content-Length", str(len(MODULE)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(MODULE)
            self.wfile.flush()
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()

        def chunk(data):
            self.wfile.write(b"%X\r\n" % len(data))
            self.wfile.write(data)
            self.wfile.write(b"\r\n")
            self.wfile.flush()

        chunk(HEAD)
        time.sleep(0.5)
        chunk(TAIL)
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def log_message(self, *args):
        pass


http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
