#!/usr/bin/env python3
"""Serve tools/pages/encoding/*.html with conflicting charset signals.

Every route exists to test one priority rule of the encoding sniffing
algorithm (see docs/hist-lessons.md R164):

    BOM  >  HTTP Content-Type charset  >  <meta charset>  >  default

The fixture files are all UTF-8; for the GBK routes this server re-encodes the
body to GBK on the fly, so the repository keeps one readable copy of each
fixture. `Cache-Control: no-store` keeps the browser's HTTP cache from
replaying a body with the previous response's headers.

Usage:
    python tools/encoding_server.py --port 8993
Then GET http://127.0.0.1:8993/<fixture>.html. Started by
tools/run_encoding_tests.ps1; not meant to be run by hand (but it is
standalone if you want to poke at it with curl).
"""

import argparse
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

PAGES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pages', 'encoding')

# path -> (Content-Type value, body transform)
ROUTES = {
    # UTF-8 bytes, UTF-8 header, stale <meta charset=gbk>: header wins.
    '/header_utf8_meta_gbk.html': ('text/html; charset=utf-8', 'utf8'),
    # GBK bytes, GBK header, stale <meta charset=utf-8>: header wins.
    '/header_gbk_meta_utf8.html': ('text/html; charset=gbk', 'gbk'),
    # UTF-8 bytes, UTF-8 header, no meta at all: header decides.
    '/no_meta_utf8.html': ('text/html; charset=utf-8', 'utf8'),
    # GBK bytes, header without charset, meta says gbk: meta decides.
    '/no_header_meta_gbk.html': ('text/html', 'gbk'),
    # UTF-8 BOM + GBK header + stale gbk meta: BOM wins.
    '/bom_utf8.html': ('text/html; charset=gbk', 'bom'),
}


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def do_GET(self):
        path = urlparse(self.path).path

        if path == '/health':
            body = b'ok'
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        route = ROUTES.get(path)
        if route is None:
            self.send_error(404, 'no such encoding fixture')
            return

        content_type, transform = route
        name = path.lstrip('/')
        with open(os.path.join(PAGES_DIR, name), 'rb') as f:
            raw = f.read()

        text = raw.decode('utf-8')
        if transform == 'gbk':
            body = text.encode('gbk')
        elif transform == 'bom':
            body = b'\xef\xbb\xbf' + text.encode('utf-8')
        else:
            body = text.encode('utf-8')

        self.send_response(200)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        # The suite prints its own PASS/FAIL lines; keep the console clean.
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8993)
    args = parser.parse_args()

    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    print('encoding server listening on http://127.0.0.1:%d' % args.port, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
