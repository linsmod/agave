#!/usr/bin/env python3
"""tools/hammer_reload_server.py

Serves /slow.html as a *progressive* response: the first half of the body is
sent, then the connection pauses, then the second half (which carries the script
that sets document.title). Reloading during the pause makes the old response
deliver a chunk after the new load has started - the case that used to feed the
previous page's bytes into the new document and leave it blank.

/chain.html is the "several reloads in a row" case: it registers a load listener
and then blocks on five slow scripts. Hammering reload across it creates more
document realms than the bridge's native-method table used to have slots for.
"""
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

FIRST = """<!doctype html><meta charset="utf-8"><title>slow</title><body>
<p id="a">first half</p>
"""
SECOND = """<p id="b">second half</p>
<script>document.title = 'ready'; document.getElementById('a').textContent = 'done';</script>
</body>
"""

CHAIN = b"""<!doctype html><meta charset="utf-8"><title>chain</title>
<script>
window.__loadCount = 0;
window.addEventListener('load', function () { window.__loadCount++; });
</script>
<p id="a">start</p>
<script src="/slow.js?i=1"></script>
<script src="/slow.js?i=2"></script>
<script src="/slow.js?i=3"></script>
<script src="/slow.js?i=4"></script>
<script src="/slow.js?i=5"></script>
<p id="b">end</p>
"""

SLOW_JS = b"window.__scripts = (window.__scripts || 0) + 1;\n"

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/chain.html'):
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(CHAIN)))
            self.end_headers()
            self.wfile.write(CHAIN)
        elif self.path.startswith('/slow.js'):
            time.sleep(0.25)
            self.send_response(200)
            self.send_header('Content-Type', 'application/javascript')
            self.send_header('Content-Length', str(len(SLOW_JS)))
            self.end_headers()
            self.wfile.write(SLOW_JS)
        elif self.path.startswith('/slow.html'):
            body = (FIRST + SECOND).encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            cut = len(FIRST.encode('utf-8'))
            self.wfile.write(body[:cut])
            self.wfile.flush()
            time.sleep(0.5)
            self.wfile.write(body[cut:])
            self.wfile.flush()
        elif self.path.startswith('/fast.html'):
            body = b"<!doctype html><title>fast</title><p>fast</p>"
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)

    def log_message(self, *args):
        pass

if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8981
    HTTPServer(('127.0.0.1', port), Handler).serve_forever()
