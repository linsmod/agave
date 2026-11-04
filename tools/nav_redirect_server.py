#!/usr/bin/env python3
"""Static file server with redirect endpoints, for navigation regression tests.

Usage: python tools/nav_redirect_server.py [port]

Serves tools/pages/ and adds:
  /redirect302 -> 302 to /navcheck_target.html
  /redirect301 -> 301 to /navcheck_target.html
  /slow        -> responds after 2s (for the [nav] stop case, if needed)
"""

import http.server
import os
import sys
import time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pages")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8973


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def do_GET(self):
        if self.path.startswith("/redirect30"):
            status = 301 if self.path.startswith("/redirect301") else 302
            self.send_response(status)
            self.send_header("Location", "/navcheck_target.html")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path.startswith("/slow"):
            time.sleep(2)
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            body = b"<html><body><p>slow</p></body></html>"
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def log_message(self, *args):
        pass


http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
