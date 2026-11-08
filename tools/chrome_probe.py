#!/usr/bin/env python3
"""Differential debugging against headless Chrome (and optionally agave).

The fixed route for "what does Chrome actually do here?" questions: one JS
snippet that ends with `console.log('RESULT ' + JSON.stringify(...))`, run in
headless Chrome through CDP and (with --agave) through the agave probe, so the
two RESULT lines can be compared directly.

Why a tool and not ad-hoc scripts: Chrome needs --headless=new, a throw-away
--user-data-dir and its own --remote-debugging-port, and the CDP session needs
Page/DOM/CSS/Runtime enabled before anything useful comes back. Every hand
written probe re-invented that (and occasionally attached to the user's own
browser). This file is the one place that starts Chrome.

Typical use:

  # same script in both engines
  python tools/chrome_probe.py --url https://chat.deepseek.com/sign_in \
      --js-file debug/divider.js --agave

  # which CSS rule wins for a selector (matched styles + the winning value)
  python tools/chrome_probe.py --url https://chat.deepseek.com/sign_in \
      --matched-styles .ds-verify-code-input-divider --props margin

  # which font actually renders a node (Chrome's platform font, e.g. Noto Sans
  # SC for the default family on a zh-CN machine)
  python tools/chrome_probe.py --url https://chat.deepseek.com/sign_in \
      --platform-fonts '#a'

  # pixel reference for the probe screenshot
  python tools/chrome_probe.py --url file:///E:/mybrowsers/agave/tools/pages/layout/x.html \
      --screenshot build-zig-native/chrome_x.png

For layout geometry across a whole page (agave vs Chrome rect diff) use
tools/layout_diff.py instead; it shares the same Chrome launch recipe.
"""
import argparse
import asyncio
import base64
import json
import os
import subprocess
import sys
import tempfile
import shutil
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CHROME_CANDIDATES = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    os.path.expandvars(r"%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe"),
]


def find_chrome():
    for path in CHROME_CANDIDATES:
        if os.path.exists(path):
            return path
    found = shutil.which("chrome") or shutil.which("chrome.exe")
    if found:
        return found
    raise SystemExit("chrome not found (looked in Program Files and LOCALAPPDATA)")


class Chrome:
    def __init__(self, width, height, port):
        self.width = width
        self.height = height
        self.port = port
        self.proc = None
        self.ws = None
        self.next_id = 1
        self.tmpdir = tempfile.mkdtemp(prefix="agb-chrome-")

    async def start(self):
        self.proc = subprocess.Popen([
            find_chrome(), "--headless=new", "--disable-gpu", "--hide-scrollbars",
            "--mute-audio", "--no-first-run", "--no-default-browser-check",
            "--disable-extensions", "--allow-file-access-from-files",
            "--window-size=%d,%d" % (self.width, self.height),
            "--remote-debugging-port=%d" % self.port,
            "--user-data-dir=" + self.tmpdir,
            "about:blank",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                urllib.request.urlopen("http://127.0.0.1:%d/json/version" % self.port, timeout=1)
                break
            except Exception:
                await asyncio.sleep(0.2)
        else:
            raise SystemExit("chrome devtools endpoint did not come up")

        import websockets  # pip: websockets

        tabs = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % self.port))
        page = [t for t in tabs if t.get("type") == "page"][0]
        self.ws = await websockets.connect(page["webSocketDebuggerUrl"], max_size=64 * 1024 * 1024)

        for method in ("Page.enable", "Runtime.enable", "DOM.enable", "CSS.enable"):
            await self.cmd(method)
        await self.cmd("Emulation.setDeviceMetricsOverride", {
            "width": self.width, "height": self.height,
            "deviceScaleFactor": 1, "mobile": False,
        })

    async def cmd(self, method, params=None):
        mid = self.next_id
        self.next_id += 1
        await self.ws.send(json.dumps({"id": mid, "method": method, "params": params or {}}))
        while True:
            reply = json.loads(await self.ws.recv())
            if reply.get("id") == mid:
                if "error" in reply:
                    raise RuntimeError("%s: %s" % (method, reply["error"]))
                return reply.get("result", {})

    async def evaluate(self, expression):
        res = await self.cmd("Runtime.evaluate", {
            "expression": expression, "returnByValue": True, "awaitPromise": True,
        })
        if "exceptionDetails" in res:
            raise RuntimeError("JS: " + json.dumps(res["exceptionDetails"])[:800])
        return res["result"].get("value")

    async def node(self, selector):
        root = (await self.cmd("DOM.getDocument", {"depth": 1}))["root"]["nodeId"]
        found = await self.cmd("DOM.querySelector", {"nodeId": root, "selector": selector})
        return found.get("nodeId")

    async def close(self):
        try:
            if self.ws:
                await self.ws.close()
        except Exception:
            pass
        if self.proc:
            self.proc.kill()
            self.proc.wait()
        shutil.rmtree(self.tmpdir, ignore_errors=True)


def read_script(args):
    if args.js:
        return args.js
    if args.js_file:
        with open(args.js_file, encoding="utf-8") as f:
            return f.read()
    return None


def capture_console_script(script):
    """Runs `script` with console.log captured, returns the captured lines."""
    return (
        "(function(){var __out=[];var __log=console.log;"
        "console.log=function(){var a=[];for(var i=0;i<arguments.length;i++)a.push(String(arguments[i]));"
        "__out.push(a.join(' '));};"
        "try{" + script + "}finally{console.log=__log;}"
        "return __out.join('\\n');})()"
    )


async def run_chrome(args, script):
    chrome = Chrome(args.width, args.height, args.port)
    await chrome.start()
    try:
        await chrome.cmd("Page.navigate", {"url": args.url})

        if args.wait_load:
            for _ in range(int(args.load_timeout * 5)):
                state = await chrome.evaluate("document.readyState")
                if state == "complete":
                    break
                await asyncio.sleep(0.2)
        await asyncio.sleep(args.wait / 1000.0)

        # Headless Chrome is not the active window: without this the page has no
        # focus, so :focus never matches, focus() paints no caret and a
        # screenshot of an input looks like an unfocused page. It has to be on
        # before the script runs (the script may focus an element).
        if args.focus_emulation:
            await chrome.cmd("Emulation.setFocusEmulationEnabled", {"enabled": True})
            await asyncio.sleep(0.1)

        if script:
            out = await chrome.evaluate(capture_console_script(script))
            print("== chrome ==")
            print(out if out else "(no console.log output)")
        elif not (args.matched_styles or args.platform_fonts or args.screenshot):
            print("== chrome ==  (nothing to do: pass --js/--js-file, --matched-styles, --platform-fonts or --screenshot)")
            return

        if args.matched_styles:
            node = await chrome.node(args.matched_styles)
            print("== chrome matched styles: %s ==" % args.matched_styles)
            if not node:
                print("(selector matched no node)")
            else:
                result = await chrome.cmd("CSS.getMatchedStylesForNode", {"nodeId": node})
                wanted = [p.strip() for p in (args.props or "").split(",") if p.strip()]

                def keep(name):
                    if not wanted:
                        return True
                    return any(name == w or name.startswith(w + "-") for w in wanted)

                for match in result.get("matchedCSSRules", []):
                    rule = match["rule"]
                    props = []
                    for prop in rule.get("style", {}).get("cssProperties", []):
                        name = prop.get("name", "")
                        if not keep(name):
                            continue
                        text = prop.get("value", "")
                        if prop.get("important"):
                            text += " !important"
                        props.append("%s: %s" % (name, text))
                    sel = rule.get("selectorList", {}).get("text", "")
                    if props or not wanted:
                        print("  %s { %s }" % (sel, "; ".join(props)))
                if args.props:
                    value = await chrome.evaluate(
                        "getComputedStyle(document.querySelector(%s)).getPropertyValue(%s)"
                        % (json.dumps(args.matched_styles), json.dumps(wanted[0] if wanted else "")))
                    print("  computed: %s = %s" % (wanted[0] if wanted else "?", value))

        if args.platform_fonts:
            node = await chrome.node(args.platform_fonts)
            print("== chrome platform fonts: %s ==" % args.platform_fonts)
            if not node:
                print("(selector matched no node)")
            else:
                fonts = await chrome.cmd("CSS.getPlatformFontsForNode", {"nodeId": node})
                for font in fonts.get("fonts", []):
                    print("  %s (postscript=%s custom=%s glyphs=%s)" % (
                        font.get("familyName"), font.get("postScriptName"),
                        font.get("isCustomFont"), font.get("glyphCount")))
                style = await chrome.evaluate(
                    "(()=>{var e=document.querySelector(%s);var cs=getComputedStyle(e);"
                    "return cs.fontFamily+' | '+cs.fontSize+' | h='+e.getBoundingClientRect().height;})()"
                    % json.dumps(args.platform_fonts))
                print("  computed: %s" % style)

        if args.screenshot:
            shot = await chrome.cmd("Page.captureScreenshot", {"format": "png"})
            path = args.screenshot
            if not os.path.isabs(path):
                path = os.path.join(ROOT, path)
            directory = os.path.dirname(path)
            if directory:
                os.makedirs(directory, exist_ok=True)
            with open(path, "wb") as f:
                f.write(base64.b64decode(shot["data"]))
            print("== chrome screenshot ==  %s" % path)
    finally:
        await chrome.close()


def run_agave(args, script):
    exe = os.path.join(ROOT, "build-zig-native", "agave_shell.exe")
    if not os.path.exists(exe):
        raise SystemExit("missing %s - run tools/build_probe.ps1 first" % exe)

    tmp_script = None
    try:
        if args.js_file:
            script_path = os.path.abspath(args.js_file)
        else:
            tmp_script = os.path.join(tempfile.gettempdir(), "agb-chrome-probe-%d.js" % os.getpid())
            with open(tmp_script, "w", encoding="utf-8") as f:
                f.write(script)
            script_path = tmp_script

        cli = [exe, "--probe", args.url, "--eval-file", script_path,
               "--wait", str(args.wait), "--settle", "300", "--timeout", str(args.timeout)]
        if args.agave_resize:
            cli += ["--resize", args.agave_resize]

        proc = subprocess.run(cli, cwd=os.path.dirname(exe), stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=args.timeout / 1000.0 + 30)
        text = (proc.stdout + proc.stderr).decode("utf-8", "replace")
        lines = []
        for line in text.splitlines():
            marker = "[con-log] "
            if marker in line:
                lines.append(line.split(marker, 1)[1])
        print("== agave ==")
        print("\n".join(lines) if lines else "(no console.log output; exit %d)" % proc.returncode)
    finally:
        if tmp_script and os.path.exists(tmp_script):
            os.remove(tmp_script)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", required=True, help="page URL (use file:///E:/... for local pages)")
    parser.add_argument("--js", help="inline JS snippet")
    parser.add_argument("--js-file", help="JS file (shared by Chrome and agave)")
    parser.add_argument("--agave", action="store_true", help="also run the snippet in the agave probe")
    parser.add_argument("--agave-resize", help="agave probe window size, e.g. 1084x677")
    parser.add_argument("--matched-styles", help="selector: dump Chrome's matched CSS rules (CDP CSS.getMatchedStylesForNode)")
    parser.add_argument("--props", help="comma separated property names to keep for --matched-styles")
    parser.add_argument("--platform-fonts", help="selector: dump the fonts Chrome actually uses (CSS.getPlatformFontsForNode)")
    parser.add_argument("--screenshot", help="write a Chrome screenshot (png)")
    parser.add_argument("--focus-emulation", action="store_true",
                        help="with --screenshot: emulate window focus so carets and :focus paint")
    parser.add_argument("--width", type=int, default=1084, help="viewport width (layout_diff uses 1084)")
    parser.add_argument("--height", type=int, default=800, help="viewport height")
    parser.add_argument("--wait", type=int, default=3000, help="ms to wait after load before evaluating")
    parser.add_argument("--wait-load", action="store_true", help="poll document.readyState until complete")
    parser.add_argument("--load-timeout", type=float, default=30.0, help="seconds for --wait-load")
    parser.add_argument("--timeout", type=int, default=60000, help="agave probe timeout in ms")
    parser.add_argument("--port", type=int, default=9450, help="Chrome remote debugging port")
    args = parser.parse_args()

    script = read_script(args)

    if script or args.matched_styles or args.platform_fonts or args.screenshot:
        asyncio.run(run_chrome(args, script))
    if args.agave:
        if not script:
            parser.error("--agave needs --js or --js-file")
        run_agave(args, script)
    if not (script or args.matched_styles or args.platform_fonts or args.screenshot or args.agave):
        parser.error("nothing to do: pass --js/--js-file, --matched-styles, --platform-fonts, --screenshot or --agave")


if __name__ == "__main__":
    main()
