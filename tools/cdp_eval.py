#!/usr/bin/env python3
"""Evaluate a JS file in a Chrome page over CDP and print the result.

Usage: python debug/cdp_eval.py <port> <js-file> [url-substring]
"""
import asyncio
import json
import sys
import urllib.request

import websockets


async def main():
    port = sys.argv[1]
    js_file = sys.argv[2]
    want = sys.argv[3] if len(sys.argv) > 3 else "v.qq.com"
    expr = open(js_file, encoding="utf-8").read()
    tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json"))
    pages = [t for t in tabs if t.get("type") == "page" and want in t.get("url", "")]
    if not pages:
        print("no matching page:", [t.get("url") for t in tabs if t.get("type") == "page"])
        return
    async with websockets.connect(pages[0]["webSocketDebuggerUrl"], max_size=32 * 1024 * 1024) as ws:
        await ws.send(json.dumps({
            "id": 1,
            "method": "Runtime.evaluate",
            "params": {"expression": expr, "returnByValue": True, "awaitPromise": True},
        }))
        while True:
            msg = json.loads(await ws.recv())
            if msg.get("id") == 1:
                res = msg["result"]
                if "exceptionDetails" in res:
                    print("EXCEPTION:", json.dumps(res["exceptionDetails"])[:2000])
                else:
                    print(res["result"].get("value", json.dumps(res["result"])))
                return


asyncio.run(main())
