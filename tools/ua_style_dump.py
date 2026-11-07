#!/usr/bin/env python3
"""Dump Chrome's AUTHORITATIVE user-agent rules for elements on a page.

Chrome's UA stylesheet (html.css) is not reachable from page JS, but the CDP
CSS domain reports matched rules with origin='user-agent'. This script opens a
page in an already running headless Chrome (--remote-debugging-port), asks for
the matched styles of the selectors passed on the command line and prints only
the UA rules plus the resolved computed values we care about.

Usage:
  python tools/ua_style_dump.py <port> <url-or-file> "<selector>" [selector ...]

Example:
  python tools/ua_style_dump.py 9333 tools/pages/cap_media.html "video" "video[controls]"

Design notes:
  * one page target per run, closed at the end (Chrome must be started with
    --remote-debugging-port; see tools/run_ua_style_probe.ps1)
  * output is deliberately plain text: `selector { prop: value; ... }` lines,
    properties in Chrome's own order, no cascaded values (that is what the
    computed value column is for).
"""
import asyncio
import json
import sys
import urllib.parse
import urllib.request

import websockets


async def cdp(ws, method, **params):
    global _next_id
    _next_id += 1
    mid = _next_id
    await ws.send(json.dumps({"id": mid, "method": method, "params": params}))
    while True:
        msg = json.loads(await ws.recv())
        if msg.get("id") == mid:
            if "error" in msg:
                raise RuntimeError(f"{method}: {msg['error']}")
            return msg.get("result", {})


_next_id = 0


def http_json(port, path, method="GET"):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method)
    with urllib.request.urlopen(req) as f:
        return json.load(f)


async def main():
    port = sys.argv[1]
    url = sys.argv[2]
    selectors = sys.argv[3:]
    if not selectors:
        print("usage: ua_style_dump.py <port> <url> <selector> [...]")
        return 2

    if not url.startswith(("http://", "https://", "file:", "data:")):
        url = "file:///" + url.replace("\\", "/")

    # Chrome >= 111 requires PUT for /json/new.
    tab = http_json(port, f"/json/new?{urllib.parse.quote(url, safe=':/?=&')}", method="PUT")
    # Chrome >= 111 requires PUT for /json/new; fall back to the default tab.
    if not isinstance(tab, dict) or "webSocketDebuggerUrl" not in tab:
        tabs = [t for t in http_json(port, "/json")
                if t.get("type") == "page"]
        if not tabs:
            print("no page target")
            return 1
        tab = tabs[0]
        ws_url = tab["webSocketDebuggerUrl"]
    else:
        ws_url = tab["webSocketDebuggerUrl"]

    async with websockets.connect(ws_url, max_size=64 * 1024 * 1024) as ws:
        await cdp(ws, "Page.enable")
        await cdp(ws, "DOM.enable")
        await cdp(ws, "CSS.enable")
        await cdp(ws, "Runtime.enable")
        if url.startswith("file:"):
            await cdp(ws, "Page.navigate", url=url)
        await asyncio.sleep(1.2)

        doc = await cdp(ws, "DOM.getDocument", depth=-1)
        root = doc["root"]["nodeId"]

        for sel in selectors:
            res = await cdp(ws, "DOM.querySelector", nodeId=root, selector=sel)
            node_id = res.get("nodeId", 0)
            print(f"\n===== {sel} (nodeId={node_id}) =====")
            if not node_id:
                print("  (no element matched)")
                continue
            try:
                matched = await cdp(ws, "CSS.getMatchedStylesForNode", nodeId=node_id)
            except RuntimeError as e:
                print(f"  matched styles failed: {e}")
                continue

            for rule in matched.get("matchedCSSRules", []):
                r = rule["rule"]
                if r.get("origin") != "user-agent":
                    continue
                sel_text = r.get("selectorList", {}).get("text", "?")
                print(f"  UA  {sel_text} {{")
                for prop in r.get("style", {}).get("cssProperties", []):
                    if prop.get("disabled"):
                        continue
                    text = prop.get("value", "").replace("\n", " ")
                    imp = " !important" if prop.get("important") else ""
                    print(f"        {prop.get('name')}: {text}{imp};")
                print("      }")

            # Computed values (what actually reaches the renderer).
            js = """(function(){
              var el = document.querySelector(%s);
              var cs = getComputedStyle(el);
              var keys = ['display','width','height','min-width','min-height',
                          'background-color','color','object-fit','vertical-align',
                          'margin-top','margin-left','overflow-x','overflow-y',
                          'aspect-ratio','border-top-width'];
              var out = [];
              for (var i=0;i<keys.length;i++) out.push(keys[i]+'='+cs.getPropertyValue(keys[i]));
              out.push('rect=' + JSON.stringify(el.getBoundingClientRect()));
              out.push('intrinsic=' + el.videoWidth + 'x' + el.videoHeight);
              return out.join(' ');
            })()""" % json.dumps(sel)
            ev = await cdp(ws, "Runtime.evaluate", expression=js, returnByValue=True)
            print("  computed: " + str(ev.get("result", {}).get("value")))

        await cdp(ws, "Page.close")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
