#!/usr/bin/env python3
"""Count text anti-aliasing coverage in a screenshot region.

Usage:
    python tools/font_aa_check.py <png|bmp> x0,y0,x1,y1 [--dark 60] [--light 240]

Prints `dark=`, `light=`, `mid=` (pixels with luminance in (dark, light)) and the
number of distinct luminance levels, then exits 0 when the region contains at
least --min-mid intermediate pixels (default 200). With 1-bit text every glyph
pixel is either the text colour or the background, so `mid` stays near 0;
grayscale anti-aliasing produces hundreds of coverage values.

The image reader is shared with pixel_check.py, so no image library is needed
(PNG and 32/24-bit BMP are both accepted).
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pixel_check import load_image  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('rect', help='x0,y0,x1,y1')
    ap.add_argument('--dark', type=int, default=60)
    ap.add_argument('--light', type=int, default=240)
    ap.add_argument('--min-mid', type=int, default=200)
    args = ap.parse_args()

    x0, y0, x1, y1 = (int(v) for v in args.rect.split(','))
    width, height, pixel = load_image(args.image)
    if x0 < 0 or y0 < 0 or x1 > width or y1 > height or x1 <= x0 or y1 <= y0:
        print('bad rect %s for image %dx%d' % (args.rect, width, height))
        return 2

    dark = light = mid = 0
    levels = set()
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = pixel(x, y)
            lum = (r * 299 + g * 587 + b * 114) // 1000
            levels.add(lum)
            if lum <= args.dark:
                dark += 1
            elif lum >= args.light:
                light += 1
            else:
                mid += 1

    print('font-aa dark=%d light=%d mid=%d levels=%d (rect %s)'
          % (dark, light, mid, len(levels), args.rect))

    if mid < args.min_mid:
        print('font-aa: FAIL (expected >= %d anti-aliased pixels, got %d)' % (args.min_mid, mid))
        return 1
    print('font-aa: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
