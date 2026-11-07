#!/usr/bin/env python3
"""Serve media files with a *wrong* Content-Type, for the media-document tests.

Chrome (and agave) decide "this top-level navigation is media" from the MIME
type, and fall back to the URL extension when the server lies. A plain
`python -m http.server` always reports the right type, so it cannot exercise
that path.

Usage:
    python tools/media_mime_server.py --root debug/media --port 8731

Then:
    http://127.0.0.1:8731/ok/clip640x360.mp4      # correct video/mp4
    http://127.0.0.1:8731/octet/clip640x360.mp4   # application/octet-stream
    http://127.0.0.1:8731/none/clip640x360.mp4    # no Content-Type at all

The probe helper uses this for the "media document" regression cases; run it
in the background and stop it when done.
"""

import argparse
import mimetypes
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse

PREFIXES = ('/ok/', '/octet/', '/none/')


class Handler(BaseHTTPRequestHandler):
    root = '.'
    chunk_delay = 0.0

    def do_GET(self):
        path = unquote(urlparse(self.path).path)
        mode = 'ok'

        for prefix in PREFIXES:
            if path.startswith(prefix):
                mode = prefix.strip('/')
                path = path[len(prefix) - 1:]
                break

        file_path = os.path.normpath(os.path.join(self.root, path.lstrip('/')))

        if not os.path.abspath(file_path).startswith(os.path.abspath(self.root)):
            self.send_error(403)
            return

        if not os.path.isfile(file_path):
            self.send_error(404)
            return

        with open(file_path, 'rb') as f:
            data = f.read()

        self.send_response(200)

        if mode == 'ok':
            self.send_header('Content-Type', mimetypes.guess_type(file_path)[0] or 'application/octet-stream')
        elif mode == 'octet':
            self.send_header('Content-Type', 'application/octet-stream')

        self.send_header('Content-Length', str(len(data)))
        self.send_header('Accept-Ranges', 'none')
        self.end_headers()

        # Count what actually leaves: a navigation that is replaced by the
        # media template aborts before the body, which shows up as a short
        # write here (that is how "no double download" is measured). Writing in
        # chunks (--chunk-delay) makes that visible even on loopback, where the
        # kernel would otherwise swallow the whole body.
        sent = 0
        chunk = 65536

        try:
            while sent < len(data):
                piece = data[sent:sent + chunk]
                written = self.wfile.write(piece)
                if not written:
                    break
                sent += written
                if Handler.chunk_delay:
                    time.sleep(Handler.chunk_delay)
        except (BrokenPipeError, ConnectionResetError):
            pass

        sys.stderr.write('[mime-server] %s %s sent=%d/%d\n'
                         % (mode, os.path.basename(file_path), sent, len(data)))
        sys.stderr.flush()

    def log_message(self, fmt, *args):
        sys.stderr.write('[mime-server] %s\n' % (fmt % args))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', default='debug/media')
    parser.add_argument('--port', type=int, default=8731)
    parser.add_argument('--chunk-delay', type=float, default=0.0,
                        help='seconds to sleep between 64 KiB chunks (makes early aborts visible)')
    args = parser.parse_args()

    Handler.root = os.path.abspath(args.root)
    Handler.chunk_delay = args.chunk_delay

    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    print('serving %s on http://127.0.0.1:%d' % (Handler.root, args.port), flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
