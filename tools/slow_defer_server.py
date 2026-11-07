#!/usr/bin/env python3
"""后代路径复测：defer / 动态 module 的脚本在解析已经结束之后才到达。

复测两条路径（post-parse flush 已封口后的到达）：

  chunk 1 (t=0)      `<head>` + `<script defer src="/slow-defer.js"></script>`
                     + `<script type="module">` 动态插入器（在 body 里以 inline
                     script 创建 module 元素，解析中插入 -> queued for parse end）
  chunk 2 (t=0.3s)   闭合 `</body></html>`，文档结束 -> flushDeferredModules 封口
  /slow-defer.js     1.5s 后送达 -> 资源回调走 queueDeferredScript 的
                     "running now (parse already finished)" 立即执行分支
  /dyn-module.js     load 后由 window.addEventListener 动态 createElement 插入，
                     走 AntRuntime::queueModule 的立即执行分支

用法： python tools/slow_defer_server.py [port]
然后： agave_shell.exe --probe http://127.0.0.1:8975/ --wait 5000  (AGB_PARSE_LOG=1)
预期： 两行 "(parse already finished)"、一行 "script (module) queued for parse end"
      （解析中的动态插入）及一行 "deferred module running"（排队的那只在 flush 跑）。
"""

import http.server
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8975

HEAD = (b"<!doctype html>\n<html><head><meta charset=\"utf-8\">"
        b"<title>slow defer</title>\n"
        b"<script defer src=\"/slow-defer.js\"></script>\n"
        b"</head>\n<body><p id=ok>page</p>\n"
        b"<script>\n"
        b"  var m = document.createElement('script');\n"
        b"  m.type = 'module';\n"                  # IDL reflection: must land on the attribute
        b"  m.src = '/dyn1.js';\n"                # dynamic insert while parsing
        b"  document.body.appendChild(m);\n"
        b"  window.addEventListener('load', function () {\n"
        b"    var s = document.createElement('script');\n"
        b"    s.type = 'module';\n"
        b"    s.src = '/dyn2.js';\n"              # dynamic insert after flush
        b"    document.body.appendChild(s);\n"
        b"  });\n"
        b"</script>\n")
TAIL = b"</body></html>\n"

DEFER_JS = b"window.__slowDeferRan = 1;\n"
MODULE_JS = b"window.__dynModuleRan = 1;\nexport {};\n"


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        if self.path.startswith("/slow-defer.js"):
            time.sleep(1.5)
            self._script(DEFER_JS)
            return
        if self.path.startswith("/dyn"):
            self._script(MODULE_JS)
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
        time.sleep(0.3)   # let the document END before the defer body arrives
        chunk(TAIL)
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def _script(self, body):
        self.send_response(200)
        self.send_header("Content-Type", "text/javascript; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()

    def log_message(self, *args):
        pass


http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()

