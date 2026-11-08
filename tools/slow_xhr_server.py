#!/usr/bin/env python3
"""tools/slow_xhr_server.py - static file server with one deliberately slow
endpoint.

Used by run_xhr_tests.ps1 for the S2 regression: a frame's XHR must be cancelled
when the frame document goes away, and proving that needs a response that
arrives *after* the frame is gone.

    python tools/slow_xhr_server.py <port> <directory> [delay_seconds]

Routes:
    /slow-xhr   -> waits `delay_seconds` (default 1.2), then responds "slow-ok"
    anything else -> served from <directory> (like python -m http.server)
"""

import http.server
import os
import socketserver
import sys
import time
import urllib.parse


class Handler(http.server.SimpleHTTPRequestHandler):
    delay = 1.2

    def __init__(self, *args, **kwargs):
        directory = kwargs.pop("directory", None)
        super().__init__(*args, directory=directory, **kwargs)

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path

        if path == "/slow-xhr":
            time.sleep(self.delay)
            body = b"slow-ok"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        super().do_GET()

    def log_message(self, fmt, *args):
        pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8991
    directory = sys.argv[2] if len(sys.argv) > 2 else "tools/pages"
    if len(sys.argv) > 3:
        Handler.delay = float(sys.argv[3])

    handler = lambda *a, **kw: Handler(*a, directory=directory, **kw)
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", port), handler) as httpd:
        httpd.serve_forever()


if __name__ == "__main__":
    main()
