#!/usr/bin/env python3
"""Multi-`Set-Cookie` response server for the cookie regression.

HTTPResponse headers may repeat `Set-Cookie`; ResourceResponse stores headers in
a map, so a plain setHTTPHeaderField() used to keep only the last one. This
server sends two in one response and the page reads them back through
document.cookie.

Usage: python tools/multi_cookie_server.py <port>
"""
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PAGE = b"""<!DOCTYPE html><html><body><script>
var x = new XMLHttpRequest();
x.open('GET', '/cookie', false);
x.send();
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path.startswith('/cookie'):
            body = b'ok'
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Set-Cookie', 'A=1; Path=/')
            self.send_header('Set-Cookie', 'B=2; Path=/')
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(PAGE)))
        self.end_headers()
        self.wfile.write(PAGE)


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8995
    HTTPServer(('127.0.0.1', port), Handler).serve_forever()
