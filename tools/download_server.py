#!/usr/bin/env python3
"""Static file server with download endpoints, for download-manager tests.

Usage: python tools/download_server.py [port]

Serves tools/pages/ and adds:
  /blob.bin   512 KiB of deterministic bytes, Content-Type: application/octet-stream
  /report     text/plain with Content-Disposition: attachment; filename="report.txt"
  /cjk        octet-stream with Content-Disposition: attachment; filename*=UTF-8''...
  /attach     a text/html page served as an attachment (must download, not render)
  /plain      a normal text/html page (must still render)
  /missing    404
"""

import http.server
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pages")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8975

# Deterministic payload: a byte pattern that a size check alone cannot fake.
BLOB = bytes((i * 7 + 13) & 0xFF for i in range(512 * 1024))
REPORT = b"download manager regression report\n"
PAGE = b"<!doctype html><html><head><title>dl plain</title></head><body>plain page</body></html>"


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def _send(self, body, content_type, disposition=None, status=200):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        if disposition:
            self.send_header("Content-Disposition", disposition)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]

        if path == "/blob.bin":
            self._send(BLOB, "application/octet-stream")
            return
        if path == "/report":
            self._send(REPORT, "text/plain", 'attachment; filename="report.txt"')
            return
        if path == "/cjk":
            # filename*=UTF-8''%E6%8A%A5%E5%91%8A%20v2.bin  ==  报告 v2.bin
            self._send(BLOB[:4096], "application/octet-stream",
                       "attachment; filename*=UTF-8''%E6%8A%A5%E5%91%8A%20v2.bin")
            return
        if path == "/attach":
            self._send(PAGE, "text/html", 'attachment; filename="attached.html"')
            return
        if path == "/plain":
            self._send(PAGE, "text/html")
            return
        if path == "/missing":
            self._send(b"no\n", "text/plain", status=404)
            return

        super().do_GET()

    def log_message(self, fmt, *args):
        sys.stderr.write("[server] " + (fmt % args) + "\n")


if __name__ == "__main__":
    with http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler) as httpd:
        sys.stderr.write("[server] listening on %d\n" % PORT)
        httpd.serve_forever()
