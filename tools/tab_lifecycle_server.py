"""tab_lifecycle_server.py - static pages + one slow font for the tab
teardown regression (tools/run_tab_lifecycle_tests.ps1).

Serves tools/pages, but /slow.ttf sleeps before answering so the font is still
in flight when the page is torn down (the R141 crash was a font that finished
*after* the document was detached).
"""
import http.server
import os
import socketserver
import sys
import time

PORT = int(sys.argv[1])
PAGES = sys.argv[2]
FONT = sys.argv[3]
DELAY = float(sys.argv[4]) if len(sys.argv) > 4 else 1.5


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=PAGES, **k)

    def do_GET(self):
        if self.path.startswith('/slow.ttf'):
            time.sleep(DELAY)
            try:
                size = os.path.getsize(FONT)
                with open(FONT, 'rb') as f:
                    self.send_response(200)
                    self.send_header('Content-Type', 'font/ttf')
                    self.send_header('Content-Length', str(size))
                    self.end_headers()
                    while True:
                        chunk = f.read(65536)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                return
            except (BrokenPipeError, ConnectionResetError):
                return
        return super().do_GET()

    def log_message(self, *a):
        pass


socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(('127.0.0.1', PORT), Handler) as httpd:
    httpd.serve_forever()
