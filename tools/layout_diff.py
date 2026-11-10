#!/usr/bin/env python3
"""Differential layout probe: agave vs headless Chrome on one page spec.

Both engines run tools/pages/layout/lib_measure.js, which reports the viewport
rect (and requested computed styles) of every element carrying data-case="...".
The viewport is pinned to 1084x800 on both sides:

  * agave: agave_shell probe window is 1100x800 and the classic scrollbar takes
    16px, so document.documentElement.clientWidth is 1084. Test pages keep
    content taller than the viewport so the scrollbar exists.
  * Chrome: --hide-scrollbars plus Emulation.setDeviceMetricsOverride(1084x800)
    gives the same client box without a scrollbar stealing space.

Parallel by design: the agave probes run in a bounded thread pool (each with its
own AGB_USERDATA and cwd) while the single Chrome instance measures page after
page, so the two sides overlap instead of adding up.

Usage:
    python tools/layout_diff.py tools/pages/layout/calc_percent.html
    python tools/layout_diff.py tools/pages/layout/*.html --tol 1.5
    python tools/layout_diff.py tools/pages/layout/flex_wrap.html --agave-only

Exit code 0 when every case is within tolerance, 1 otherwise.
"""
import argparse
import asyncio
import concurrent.futures
import glob
import json
import os
import random
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AGAVE = os.path.join(ROOT, "build-zig-native", "agave_shell.exe")
MEASURE_JS = os.path.join(ROOT, "tools", "pages", "layout", "lib_measure.js")
CHROME_CANDIDATES = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    os.path.expandvars(r"%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe"),
]

VIEW_W, VIEW_H = 1084, 800
PROBE_WORKERS = int(os.environ.get("AGB_LAYOUT_PROBES", "6"))
EXIT_RE = re.compile(r"^(?:\[[^]]*\]\s*)?\[exit\]\s?(.*)$")
STAMP_RE = re.compile(r"^\[[ 0-9.]+ms\]")


def find_chrome():
    for p in CHROME_CANDIDATES:
        if os.path.isfile(p):
            return p
    found = shutil.which("chrome") or shutil.which("chrome.exe")
    if found:
        return found
    raise SystemExit("chrome not found")


def file_url(path):
    return "file:///" + os.path.abspath(path).replace("\\", "/")


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# The probes share tools/TestParallel.psm1's cross-suite gate (slot files under
# build-zig-native/test-runs/.gate) so a geometry page cannot push the whole
# batch past AGB_TEST_MAX_PROBES.
GATE_DIR = os.path.join(ROOT, "build-zig-native", "test-runs", ".gate")
GATE_MAX = int(os.environ.get("AGB_TEST_MAX_PROBES", "16"))


def gate_slot():
    os.makedirs(GATE_DIR, exist_ok=True)
    deadline = time.time() + 180
    while True:
        now = time.time()
        for name in os.listdir(GATE_DIR):
            if not name.startswith("slot-"):
                continue
            path = os.path.join(GATE_DIR, name)
            try:
                if now - os.path.getmtime(path) > 900:
                    os.remove(path)
            except OSError:
                pass
        slot = os.path.join(GATE_DIR, "slot-%d-%s" % (os.getpid(), uuid.uuid4().hex))
        try:
            open(slot, "w").close()
        except OSError:
            return None
        count = len([n for n in os.listdir(GATE_DIR) if n.startswith("slot-")])
        if count <= GATE_MAX:
            return slot
        try:
            os.remove(slot)
        except OSError:
            pass
        if time.time() > deadline:
            return None
        time.sleep(0.03 + random.random() * 0.05)


def gate_release(slot):
    if slot:
        try:
            os.remove(slot)
        except OSError:
            pass


class AgavePage:
    """One probe run per page, with throwaway AGB_USERDATA / cwd / diag dir."""

    def __init__(self, page_path, wait_ms):
        self.page = page_path
        self.wait_ms = wait_ms
        # The probe runs in the harness sandbox: only the build tree is
        # guaranteed writable for it, so profiles/diagnostics live there.
        base = os.path.join(ROOT, "build-zig-native", "test-runs")
        os.makedirs(base, exist_ok=True)
        self.workdir = tempfile.mkdtemp(prefix="layout-probe-", dir=base)

    def run(self):
        env = dict(os.environ)
        env["AGB_USERDATA"] = os.path.join(self.workdir, "profile")
        env["AGB_DIAG_DIR"] = os.path.join(self.workdir, "diag")
        os.makedirs(env["AGB_USERDATA"], exist_ok=True)
        os.makedirs(env["AGB_DIAG_DIR"], exist_ok=True)
        slot = gate_slot()
        try:
            proc = subprocess.Popen(
                [AGAVE, "--probe", file_url(self.page), "--eval-file", MEASURE_JS,
                 "--wait", str(self.wait_ms), "--settle", "200", "--timeout", "40000"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, encoding="utf-8", errors="replace",
                cwd=self.workdir, env=env,
            )
            # Tie the slot to the probe's own pid (the suites' shared gate
            # prunes slots whose probe is gone, even if the owner is still busy
            # waiting for the next slot).
            if slot:
                try:
                    with open(slot, "w") as f:
                        f.write(str(proc.pid))
                except OSError:
                    pass
            try:
                stdout, stderr = proc.communicate(timeout=90)
            except subprocess.TimeoutExpired:
                proc.kill()
                stdout, stderr = proc.communicate()
            out = (stdout or "") + "\n" + (stderr or "")
            lines = out.splitlines()
            payload = None
            for i, line in enumerate(lines):
                m = EXIT_RE.match(line.strip())
                if m:
                    body = [m.group(1)]
                    for follow in lines[i + 1:]:
                        # probeExit exits the process right away; whatever follows
                        # is part of the multi-line payload unless it is a log line.
                        if STAMP_RE.match(follow) or follow.startswith("[shell]"):
                            break
                        body.append(follow)
                    payload = "\n".join(body).strip("\n")
                    break
            if payload is None:
                raise SystemExit("agave produced no [exit] payload (exit %d):\n%s" % (
                    proc.returncode, out[-3000:]))
            return payload, proc.returncode
        finally:
            gate_release(slot)
            shutil.rmtree(self.workdir, ignore_errors=True)


class Chrome:
    def __init__(self):
        self.port = free_port()
        self.proc = None
        self.ws = None
        self.tmpdir = tempfile.mkdtemp(prefix="agb-layout-")

    async def start(self):
        self.proc = subprocess.Popen([
            find_chrome(), "--headless=new", "--disable-gpu", "--hide-scrollbars",
            "--mute-audio", "--no-first-run", "--no-default-browser-check",
            "--disable-extensions", "--allow-file-access-from-files",
            "--window-size=%d,%d" % (VIEW_W, VIEW_H),
            "--remote-debugging-port=%d" % self.port,
            "--user-data-dir=" + self.tmpdir,
            "about:blank",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                urllib.request.urlopen("http://127.0.0.1:%d/json/version" % self.port, timeout=1)
                break
            except Exception:
                await asyncio.sleep(0.15)
        else:
            raise SystemExit("chrome devtools endpoint did not come up")

        import websockets
        tabs = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % self.port))
        page = [t for t in tabs if t.get("type") == "page"][0]
        self.ws = await websockets.connect(page["webSocketDebuggerUrl"], max_size=32 * 1024 * 1024)
        await self.cmd("Page.enable")
        await self.cmd("Runtime.enable")
        await self.cmd("Emulation.setDeviceMetricsOverride", {
            "width": VIEW_W, "height": VIEW_H, "deviceScaleFactor": 1, "mobile": False,
        })
        self.next_id = 1

    async def cmd(self, method, params=None, session_id=None):
        self.next_id = getattr(self, "next_id", 1)
        mid = self.next_id
        self.next_id += 1
        msg = {"id": mid, "method": method, "params": params or {}}
        if session_id:
            msg["sessionId"] = session_id
        await self.ws.send(json.dumps(msg))
        while True:
            reply = json.loads(await self.ws.recv())
            if reply.get("id") == mid:
                if "error" in reply:
                    raise RuntimeError("%s: %s" % (method, reply["error"]))
                return reply.get("result", {})

    async def evaluate(self, expr):
        res = await self.cmd("Runtime.evaluate", {
            "expression": expr, "returnByValue": True, "awaitPromise": True,
        })
        if "exceptionDetails" in res:
            raise RuntimeError("JS: " + json.dumps(res["exceptionDetails"])[:800])
        val = res["result"].get("value")
        return "" if val is None else str(val)

    async def measure(self, page_path):
        await self.cmd("Page.navigate", {"url": file_url(page_path)})
        # File pages are ready almost immediately; poll readyState instead of
        # sleeping a fixed 350 ms per page (21 pages == 7 s of pure sleep).
        deadline = time.time() + 8
        while time.time() < deadline:
            try:
                state = await self.evaluate("document.readyState")
            except RuntimeError:
                state = "loading"
            if state == "complete":
                break
            await asyncio.sleep(0.01)
        with open(MEASURE_JS, encoding="utf-8") as f:
            await self.evaluate(f.read())
        return await self.evaluate("__measure()")

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


CASE_RE = re.compile(r"^([A-Za-z0-9_.-]+):(-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)(.*)$")


def parse_payload(text):
    rows = {}
    order = []
    for line in text.splitlines():
        line = line.strip()
        m = CASE_RE.match(line)
        if m:
            name = m.group(1)
            rows[name] = {
                "rect": [float(m.group(i)) for i in (2, 3, 4, 5)],
                "props": m.group(6),
                "raw": line,
            }
            order.append(name)
        elif line.startswith("viewport:"):
            rows["viewport"] = {"rect": None, "props": "", "raw": line}
            order.append("viewport")
    return rows, order


def compare(page, agave_text, chrome_text, tol):
    a, a_order = parse_payload(agave_text)
    c, c_order = parse_payload(chrome_text) if chrome_text is not None else ({}, [])
    ok = True
    print("== %s" % os.path.basename(page))
    for name in a_order:
        if name == "viewport":
            print("   %-4s agave %s" % ("vp", a[name]["raw"]))
            if c:
                print("        chrome %s %s" % ("", c.get("viewport", {}).get("raw", "")))
            continue
        if name not in c:
            print("   %-4s agave %s (chrome: missing)" % (name, a[name]["raw"]))
            continue
        ar, cr = a[name]["rect"], c[name]["rect"]
        deltas = [ar[i] - cr[i] for i in range(4)]
        bad = any(abs(d) > tol for d in deltas)
        probs = ""
        if a[name]["props"] != c[name]["props"]:
            bad = True
            probs = "  props agave%s chrome%s" % (a[name]["props"], c[name]["props"])
        if bad:
            ok = False
            print("   FAIL %-4s agave %s" % (name, a[name]["raw"]))
            print("        chrome %s%s" % (c[name]["raw"], probs))
            print("        delta  %.2f,%.2f,%.2f,%.2f" % tuple(deltas))
        else:
            print("   ok   %-4s %s" % (name, a[name]["raw"]))
    return ok


async def main_async(args):
    pages = []
    for pat in args.pages:
        hits = glob.glob(pat) or [pat]
        pages.extend(hits)
    if not pages:
        raise SystemExit("no pages")

    chrome = None
    if not args.agave_only:
        chrome = Chrome()
        await chrome.start()
    try:
        all_ok = True
        # Agave probes run in the background while Chrome walks the same pages.
        with concurrent.futures.ThreadPoolExecutor(max_workers=PROBE_WORKERS) as pool:
            futures = {page: pool.submit(AgavePage(page, args.wait).run) for page in pages}
            for page in pages:
                agave_text, code = futures[page].result()
                chrome_text = await chrome.measure(page) if chrome else None
                all_ok &= compare(page, agave_text, chrome_text, args.tol)
                if code not in (0,):
                    all_ok = False
                    print("   agave exit %d" % code)
    finally:
        if chrome:
            await chrome.close()
    return 0 if all_ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pages", nargs="+")
    ap.add_argument("--tol", type=float, default=1.0, help="pixel tolerance")
    ap.add_argument("--wait", type=int, default=400)
    ap.add_argument("--agave-only", action="store_true")
    args = ap.parse_args()
    sys.exit(asyncio.run(main_async(args)))


if __name__ == "__main__":
    main()
