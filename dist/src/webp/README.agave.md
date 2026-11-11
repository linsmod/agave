# libwebp (vendored, decode side only)

- Upstream: https://chromium.googlesource.com/webm/libwebp
- Version: **1.4.0** (release tarball `libwebp-1.4.0.tar.gz`, storage.googleapis.com)
- License: **BSD-3-Clause** (see `COPYING`, `PATENTS`); agave is BSD-3-Clause too, so
  the license is compatible. This directory only holds the decoder side.
- Imported for: `issues #19` — the image pipeline had no WebP decoder, so every
  `.webp` resource decoded to nothing (sizeAvail=0, broken image).

## What is here

```
dist/src/webp/src/dec/*       VP8 / VP8L / VP8X frame decoding
dist/src/webp/src/dsp/*       C kernels + the SSE2/SSE41/NEON/MSA/MIPS variants
                              (the arch-specific ones compile to nothing unless the
                              matching WEBP_USE_* macro is set)
dist/src/webp/src/utils/*     bit reader, colour cache, huffman, palette, ...
dist/src/webp/src/webp/*      public headers (<webp/decode.h> etc.)
dist/src/webp/src/enc/*.h     headers only: dsp/lossless.h includes histogram_enc.h
```

Dropped: `enc/*.c`, `demux`, `mux`, `imageio`, `examples`, `extras`, the build
system files. Nothing in the picture pipeline encodes WebP.

## How it is built

`dist/allinone.cmake` globs `dist/src/*.c` recursively (with `CONFIGURE_DEPENDS`),
so these sources compile into the `dist` static library; the decoder in
`src/webcore/platform/image-decoders/webp/` uses `<webp/decode.h>` from
`dist/src/webp/src` (include path added in `src/src.cmake`).

Feature flag: `OPT_WEBP_SUPPORT` (default ON) -> `WEBP_SUPPORT`, and the macro is
declared in **`build/mconfig.h.in`** — a new vendored library needs a
`#cmakedefine` line there or the code is compiled out silently.

## Updating

1. Download the new release tarball, extract `src/{dec,dsp,utils,webp}` and
   `src/enc/*.h` here (keep the `src/` prefix: libwebp sources include each other
   as `"src/..."`).
2. Keep `COPYING`/`PATENTS` in sync.
3. Rebuild; `tools/run_webp_tests.ps1` is the gate (three fixtures, 40x30 each).
