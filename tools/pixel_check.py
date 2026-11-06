#!/usr/bin/env python3
"""Assert pixels in a probe screenshot (BMP, 32/24-bit, bottom-up).

Usage:
    python tools/pixel_check.py shot.bmp 250,150,ff0000 450,150,ffffff --tol 8
    python tools/pixel_check.py shot.bmp 0,0,55,139,ffffff,100

Two spec forms (screenshot coordinates, origin top-left):

    x,y,RRGGBB                        one pixel must match
    x0,y0,x1,y1,RRGGBB,MIN            the rect must hold >= MIN matching pixels

The second form is for content whose exact position depends on font metrics
(text ink) where the interesting question is "did anything get drawn at all".

Exit code 1 when any spec fails, so this doubles as a paint regression check
(layout_diff covers geometry; this covers what actually got drawn).
"""
import struct
import sys


def load_bmp(path):
    with open(path, "rb") as f:
        data = f.read()

    if data[:2] != b"BM":
        raise SystemExit("%s: not a BMP" % path)

    offset = struct.unpack_from("<I", data, 10)[0]
    header_size = struct.unpack_from("<I", data, 14)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]

    if header_size < 40 or bpp not in (24, 32):
        raise SystemExit("%s: unsupported BMP (bpp=%d)" % (path, bpp))

    top_down = height < 0
    height = abs(height)
    stride = ((width * bpp // 8) + 3) & ~3

    def pixel(x, y):
        row = y if top_down else (height - 1 - y)
        off = offset + row * stride + x * (bpp // 8)
        b, g, r = data[off], data[off + 1], data[off + 2]
        return r, g, b

    return width, height, pixel


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)

    path = sys.argv[1]
    tol = 8
    specs = []
    args = sys.argv[2:]
    i = 0
    while i < len(args):
        if args[i] == "--tol":
            tol = int(args[i + 1])
            i += 2
            continue
        specs.append(args[i])
        i += 1

    width, height, pixel = load_bmp(path)
    failures = 0

    def close(got, want):
        return all(abs(got[c] - want[c]) <= tol for c in range(3))

    for spec in specs:
        fields = spec.split(",")
        try:
            if len(fields) == 6:
                x0, y0, x1, y1 = (int(v) for v in fields[:4])
                want = tuple(int(fields[4][k:k + 2], 16) for k in (0, 2, 4))
                minimum = int(fields[5])
            elif len(fields) == 3:
                x0, y0 = (int(v) for v in fields[:2])
                x1, y1 = x0, y0
                want = tuple(int(fields[2][k:k + 2], 16) for k in (0, 2, 4))
                minimum = None
            else:
                raise ValueError("field count")
        except Exception:
            raise SystemExit("bad pixel spec %r (want x,y,RRGGBB or x0,y0,x1,y1,RRGGBB,MIN)" % spec)

        if not (0 <= x0 < width and 0 <= y0 < height and 0 <= x1 < width and 0 <= y1 < height):
            print("FAIL %-28s out of bounds (%dx%d)" % (spec, width, height))
            failures += 1
            continue

        if minimum is None:
            got = pixel(x0, y0)
            if close(got, want):
                print("ok   %-28s got rgb%s" % (spec, got))
            else:
                failures += 1
                print("FAIL %-28s got rgb%s want rgb%s" % (spec, got, want))
            continue

        found = 0
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if close(pixel(x, y), want):
                    found += 1
        if found >= minimum:
            print("ok   %-28s %d >= %d matching pixels" % (spec, found, minimum))
        else:
            failures += 1
            print("FAIL %-28s only %d matching pixels, want >= %d" % (spec, found, minimum))

    print("%d/%d checks ok" % (len(specs) - failures, len(specs)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
