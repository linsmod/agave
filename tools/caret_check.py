#!/usr/bin/env python3
"""Measure the caret in an agave screenshot taken with AGB_CARET_DEBUG=1.

That switch paints the caret red even when the frame has no focus and no matter
where the blink phase is, so a headless screenshot always has one and it cannot
be confused with text ink or a border.

Usage:
    python tools/caret_check.py build-zig-native/caret.png [--rect x,y,w,h]

Prints `caret x=<x> y=<y0>..<y1> height=<h>` and exits 0; exit 1 when no red
caret is found. The caller decides whether the height is right (the regression
asserts it is the text height, not the input's/line box's).
"""
import argparse
import sys

from PIL import Image


def is_caret(pixel):
    r, g, b = pixel
    return r > 150 and g < 120 and b < 120


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("png")
    parser.add_argument("--rect", help="x,y,w,h in CSS pixels (default: whole image)")
    args = parser.parse_args()

    im = Image.open(args.png).convert("RGB")
    if args.rect:
        x, y, w, h = (int(v) for v in args.rect.split(","))
        im = im.crop((x, y, x + w, y + h))

    px = im.load()
    width, height = im.size
    hits = [(x, y) for x in range(width) for y in range(height) if is_caret(px[x, y])]

    if not hits:
        print("no caret found in %s" % args.png)
        return 1

    xs = [p[0] for p in hits]
    ys = [p[1] for p in hits]
    print("caret x=%d y=%d..%d height=%d" % (xs[0], min(ys), max(ys), max(ys) - min(ys) + 1))
    print("caret columns=%d..%d" % (min(xs), max(xs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
