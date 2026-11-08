/*
 * Copyright (C) 2004, 2005, 2006 Apple Computer, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#ifndef IMAGE_SOURCE_H_
#define IMAGE_SOURCE_H_

#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

#include <picasso/picasso.h>

#include "IntSize.h"

namespace WebCore {

class SharedBuffer;

class ImageDecoder;
class ImageSource;
class PixelsBuffer;

class ImageRef : Noncopyable
{
public: 
	ImageRef()
		: source(0)
		, index(0)
		, width(0)
		, height(0)
		, naturalWidth(0)
		, naturalHeight(0)
		, pitch(0)
		, transparent(false)
		, colorkey(false)
	{
	}

	~ImageRef()
	{
	}

	ImageSource* source;
	size_t index;
	/* Pixel-buffer geometry. With a scaled decode (L3) this can be smaller than
	 * the image's natural size, which is what layout uses. */
	int width;
	int height;
	int naturalWidth;
	int naturalHeight;
	int pitch;
	bool transparent;
	bool colorkey;
};


typedef ImageRef* NativeImagePtr;
typedef ImageDecoder* NativeImageSourcePtr;

const int cAnimationLoopOnce = -1;
const int cAnimationNone = -2;

class ImageSource : Noncopyable {
public:
    ImageSource();
    ~ImageSource();

    void clear();

    bool initialized() const;
    
    void setData(SharedBuffer* data, bool allDataReceived);

    bool isSizeAvailable();
    IntSize size() const;
    
    int repetitionCount();
    
    size_t frameCount() const;
    
    NativeImagePtr createFrameAtIndex(size_t);

    /* The decoder's pixels for one frame, decoded with this paint's budget
     * applied (docs/design-async-image-decode.md). Paint code that needs the
     * raw buffer - ImagePS's scaled draw and pattern fill - must come through
     * here instead of decoder()->frameBufferAtIndex(), otherwise it skips the
     * budget and decodes a whole image inside one frame. */
    PixelsBuffer* frameBufferAtIndex(size_t index);

    /* True while the decoder still owes rows for the frame: the owner should
     * come back for another slice (BitmapImage re-arms its timer). */
    bool frameHasMoreWork() const;

    /* Tell the decoder how big the image is drawn, in device pixels, so formats
     * that can decode smaller than natural (JPEG) stop wasting work (L3).
     * Returns true when the already decoded frame is now too small for that
     * display size, i.e. the caller should drop it and decode again. */
    bool setScaleHint(const IntSize& displaySize);

    /* Geometry of the pixels actually decoded (0x0 when nothing is decoded).
     * Smaller than size() when the decoder used a scale (L3). */
    IntSize decodedFrameSize(size_t index) const;

    /* L2: install a frame decoded by ImageDecodeService (takes ownership). The
     * engine-side decoder is no longer used for painting; all the frame
     * accessors below serve this buffer instead. */
    void adoptDecodedFrame(PixelsBuffer* frame);
    bool hasDecodedFrame() const { return m_adoptedFrame != 0; }
    void clearDecodedFrame();

    /* Callers that are not inside a paint traversal (canvas drawImage, printing)
     * need the whole frame right now and have nowhere to repaint later, so they
     * bypass the per-paint decode budget. */
    void setDecodeBudgetIgnored(bool ignored) { m_ignoreDecodeBudget = ignored; }
    
    float frameDurationAtIndex(size_t);
    bool frameHasAlphaAtIndex(size_t); // Whether or not the frame actually used any alpha.
    bool frameIsCompleteAtIndex(size_t); // Whether or not the frame is completely decoded.

	//macross added
	NativeImageSourcePtr decoder(void) const { return m_decoder; }
private:
    void applyScaleHint();

    NativeImageSourcePtr m_decoder;
    PixelsBuffer* m_adoptedFrame; // L2: frame decoded off the engine thread
    IntSize m_adoptedNaturalSize; // natural size when the frame was adopted
    bool m_ignoreDecodeBudget;
    IntSize m_scaleHint; // Display size in device pixels (0x0 = natural size).
};

}

#endif
