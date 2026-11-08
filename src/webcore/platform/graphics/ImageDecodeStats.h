/*
 * ImageDecodeStats.h: how much of a frame the image decoders took.
 *
 * Copyright (C) 2026 agave project
 *
 * The decoders are lazy: paint asks for pixels and libjpeg/libpng/GIF do the
 * work inside that call (ImageSource::createFrameAtIndex ->
 * ImageDecoder::frameBufferAtIndex). So "paint=467ms" on a 400x300 dirty rect
 * looks like a paint bug, when it is one image decode. Reset once per frame,
 * then report the total next to layout/paint time.
 *
 * Engine-thread only: paint, layout and the decoders all run on that thread, so
 * this is a plain double with no lock. A background decode service (see
 * docs/design-async-image-decode.md) would have to stop adding to it.
 */

#ifndef ImageDecodeStats_h
#define ImageDecodeStats_h

namespace WebCore {

/* Monotonic seconds: use it around a decode call to measure it. */
double imageDecodeStatsNow();

void imageDecodeStatsAddMs(double ms);

/* Milliseconds accumulated since the last reset (never consumes). */
double imageDecodeStatsPeekMs();

/* paintView() brackets its layout+paint with these: inside a frame the decode
 * budget is shared by every image (a page with 20 images must not decode 20 x
 * budget), outside one - a JS-driven decode(), an animation timer - a call gets
 * a fresh budget instead of inheriting an exhausted frame. */
void imageDecodeStatsBeginFrame();
void imageDecodeStatsEndFrame();
bool imageDecodeStatsInFrame();

}

#endif // ImageDecodeStats_h
