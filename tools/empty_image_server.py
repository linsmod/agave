"""Serve tools/pages plus the "image with no body" endpoints used by
tools/pages/cap_empty_image.html.

`python -m http.server` always sends a body, so it cannot produce the responses
that used to crash the decoder (R140):

    /empty.png      200 with Content-Length: 0      (no data at all)
    /nocontent.png  204 No Content                  (no body, no length)
    /truncated.png  200 with 6 bytes of PNG header  (sniffed, size never arrives)

Everything else is served from tools/pages, so cap_empty_image.html can use
cap_pixel.png (a real 1x1 PNG) as the valid image next to the broken ones.

Usage: python tools/empty_image_server.py <port>
"""

import http.server
import os
import sys

PAGES = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pages')


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=PAGES, **kwargs)

    def log_message(self, *args):
        pass

    def _send(self, status, body=b'', length=True):
        self.send_response(status)
        if length:
            self.send_header('Content-Type', 'image/png')
            self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        path = self.path.split('?')[0]

        if path == '/empty.png':
            self._send(200, b'')
        elif path == '/nocontent.png':
            self._send(204, length=False)
        elif path == '/truncated.png':
            self._send(200, b'\x89PNG\r\n')
        else:
            super().do_GET()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8917
    server = http.server.ThreadingHTTPServer(('127.0.0.1', port), Handler)
    print('READY %d' % port, flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
