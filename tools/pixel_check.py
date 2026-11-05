#!/usr/bin/env python3
"""Assert pixels in a probe screenshot (BMP, 32/24-bit, bottom-up).

Usage:
    python tools/pixel_check.py shot.bmp 250,150,ff0000 450,150,ffffff --tol 8

Each spec is x,y,RRGGBB (screenshot coordinates, origin top-left). Exit code 1
when any pixel is outside the tolerance, so this doubles as a paint regression
check (layout_diff covers geometry; this covers what actually got drawn).
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

    for spec in specs:
        try:
            xs, ys, hexrgb = spec.split(",")
            x, y = int(xs), int(ys)
            want = tuple(int(hexrgb[k:k + 2], 16) for k in (0, 2, 4))
        except Exception:
            raise SystemExit("bad pixel spec %r (want x,y,RRGGBB)" % spec)

        if not (0 <= x < width and 0 <= y < height):
            print("FAIL %-16s out of bounds (%dx%d)" % (spec, width, height))
            failures += 1
            continue

        got = pixel(x, y)
        if all(abs(got[c] - want[c]) <= tol for c in range(3)):
            print("ok   %-16s got rgb%s" % (spec, got))
        else:
            failures += 1
            print("FAIL %-16s got rgb%s want rgb%s" % (spec, got, want))

    print("%d/%d pixels ok" % (len(specs) - failures, len(specs)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
