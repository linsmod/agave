/*
 * Copyright (C) 2006 Apple Computer, Inc.
 *
 * Portions are Copyright (C) 2001-6 mozilla.org
 *
 * Other contributors:
 *   Stuart Parmenter <stuart@mozilla.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Alternatively, the contents of this file may be used under the terms
 * of either the Mozilla Public License Version 1.1, found at
 * http://www.mozilla.org/MPL/ (the "MPL") or the GNU General Public
 * License Version 2.0, found at http://www.fsf.org/copyleft/gpl.html
 * (the "GPL"), in which case the provisions of the MPL or the GPL are
 * applicable instead of those above.  If you wish to allow use of your
 * version of this file only under the terms of one of those two
 * licenses (the MPL or the GPL) and not to allow others to use your
 * version of this file under the LGPL, indicate your decision by
 * deletingthe provisions above and replace them with the notice and
 * other provisions required by the MPL or the GPL, as the case may be.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under any of the LGPL, the MPL or the GPL.
 */

#include "JPEGImageDecoder.h"

#ifdef JPEG_SUPPORT

#include <assert.h>
#include <stdlib.h>

extern "C" {
#include <stdio.h>
#include "jpeglib.h"
}

#include <setjmp.h>

using namespace std;

namespace WebCore {

struct decoder_error_mgr {
    struct jpeg_error_mgr pub;  /* "public" fields for IJG library*/
    jmp_buf setjmp_buffer;      /* For handling catastropic errors */
};

enum jstate {
    JPEG_HEADER,                          /* Reading JFIF headers */
    JPEG_START_DECOMPRESS,
    JPEG_DECOMPRESS_PROGRESSIVE,          /* Output progressive pixels */
    JPEG_DECOMPRESS_SEQUENTIAL,           /* Output sequential pixels */
    JPEG_DONE,
    JPEG_SINK_NON_JPEG_TRAILER,          /* Some image files have a */
                                         /* non-JPEG trailer */
    JPEG_ERROR    
};

void init_source(j_decompress_ptr jd);
boolean fill_input_buffer(j_decompress_ptr jd);
void skip_input_data(j_decompress_ptr jd, long num_bytes);
void term_source(j_decompress_ptr jd);
void error_exit(j_common_ptr cinfo);

/*
 *  Implementation of a JPEG src object that understands our state machine
 */
struct decoder_source_mgr {
  /* public fields; must be first in this struct! */
  struct jpeg_source_mgr pub;

  JPEGImageReader *decoder;
};

class JPEGImageReader
{
public:
    JPEGImageReader(JPEGImageDecoder* decoder)
        : m_decoder(decoder)
        , m_bufferLength(0)
        , m_bytesToSkip(0)
        , m_state(JPEG_HEADER)
        , m_samples(0)
        , m_scaleDenom(1)
    {
        memset(&m_info, 0, sizeof(jpeg_decompress_struct));
 
        /* We set up the normal JPEG error routines, then override error_exit. */
        m_info.err = jpeg_std_error(&m_err.pub);
        m_err.pub.error_exit = error_exit;

        /* Allocate and initialize JPEG decompression object */
        jpeg_create_decompress(&m_info);
  
        decoder_source_mgr* src = NULL;
        if (!m_info.src) {
            src = (decoder_source_mgr*)fastCalloc(sizeof(decoder_source_mgr), 1);
            if (!src) {
                m_state = JPEG_ERROR;
                return;
            }
        }

        m_info.src = (jpeg_source_mgr*)src;

        /* Set up callback functions. */
        src->pub.init_source = init_source;
        src->pub.fill_input_buffer = fill_input_buffer;
        src->pub.skip_input_data = skip_input_data;
        src->pub.resync_to_restart = jpeg_resync_to_restart;
        src->pub.term_source = term_source;
        src->decoder = this;
    }

    ~JPEGImageReader()
    {
        close();
    }

    void close() {
        decoder_source_mgr* src = (decoder_source_mgr*)m_info.src;
        if (src)
            fastFree(src);
        m_info.src = 0;

        jpeg_destroy_decompress(&m_info);
    }

    void skipBytes(long num_bytes) {
        decoder_source_mgr* src = (decoder_source_mgr*)m_info.src;
        long bytesToSkip = min(num_bytes, (long)src->pub.bytes_in_buffer);
        src->pub.bytes_in_buffer -= (size_t)bytesToSkip;
        src->pub.next_input_byte += bytesToSkip;
    
        if (num_bytes > bytesToSkip)
            m_bytesToSkip = (size_t)(num_bytes - bytesToSkip);
        else
            m_bytesToSkip = 0;
    }

    bool decode(const Vector<char>& data, bool sizeOnly) {
        m_decodingSizeOnly = sizeOnly;
        
        unsigned newByteCount = data.size() - m_bufferLength;
        unsigned readOffset = m_bufferLength - m_info.src->bytes_in_buffer;

        m_info.src->bytes_in_buffer += newByteCount;
        m_info.src->next_input_byte = (JOCTET*)(data.data()) + readOffset;
        
        // If we still have bytes to skip, try to skip those now.
        if (m_bytesToSkip)
            skipBytes(m_bytesToSkip);

        m_bufferLength = data.size();
        
        // We need to do the setjmp here. Otherwise bad things will happen
        if (setjmp(m_err.setjmp_buffer)) {
            m_state = JPEG_SINK_NON_JPEG_TRAILER;
            close();
            return false;
        }

        switch (m_state) {
            case JPEG_HEADER:
            {
                /* Read file parameters with jpeg_read_header() */
                if (jpeg_read_header(&m_info, true) == JPEG_SUSPENDED)
                    return true; /* I/O suspension */

                /* let libjpeg take care of gray->RGB and YCbCr->RGB conversions */
                switch (m_info.jpeg_color_space) {
                    case JCS_GRAYSCALE:
                    case JCS_RGB:
                    case JCS_YCbCr:
                        m_info.out_color_space = JCS_RGB;
                        break;
                    case JCS_CMYK:
                    case JCS_YCCK:
                    default:
                        m_state = JPEG_ERROR;
                        return false;
                }

                /*
                 * Don't allocate a giant and superfluous memory buffer
                 * when the image is a sequential JPEG.
                 */
                m_info.buffered_image = jpeg_has_multiple_scans(&m_info);

                /* L3: the scale hint can arrive after the header (the first
                 * draw knows the display size), so the denom is chosen in the
                 * START_DECOMPRESS state below, right before libjpeg starts.
                 * Here only make sure the buffer is available. */
                /* Used to set up image size so arrays can be allocated */
                jpeg_calc_output_dimensions(&m_info);

                /*
                 * Make a one-row-high sample array that will go away
                 * when done with image. Always make it big enough to
                 * hold an RGB row.  Since this uses the IJG memory
                 * manager, it must be allocated before the call to
                 * jpeg_start_compress().
                 */
                int row_stride = m_info.output_width * 4; // RGBA buffer


                m_samples = (*m_info.mem->alloc_sarray)((j_common_ptr) &m_info,
                                           JPOOL_IMAGE,
                                           row_stride, 1);

                m_state = JPEG_START_DECOMPRESS;

                // We can fill in the size now that the header is available.
                m_decoder->setSize(m_info.image_width, m_info.image_height);

                if (m_decodingSizeOnly) {
                    // We can stop here.
                    // Reduce our buffer length and available data.
                    m_bufferLength -= m_info.src->bytes_in_buffer;
                    m_info.src->bytes_in_buffer = 0;
                    return true;
                }
            }

            case JPEG_START_DECOMPRESS:
            {
                /* Set parameters for decompression */
                /* FIXME -- Should reset dct_method and dither mode
                 * for final pass of progressive JPEG
                 */
                m_info.dct_method =  JDCT_ISLOW;
                m_info.dither_mode = JDITHER_FS;
                m_info.do_fancy_upsampling = true;
                m_info.enable_2pass_quant = false;
                m_info.do_block_smoothing = true;

                /* L3: choose the DCT scale now. The display-size hint can arrive
                 * after the header (the first draw is what knows the display
                 * size), so the header pass always computes natural output and
                 * this state overrides it just before libjpeg starts. */
                if (!m_decodingSizeOnly) {
                    int denom = m_decoder->scaleDenomFor(
                        IntSize(m_info.image_width, m_info.image_height));

                    if (denom > 1) {
                        m_info.scale_num = 1;
                        m_info.scale_denom = denom;
                        jpeg_calc_output_dimensions(&m_info);
                    }
                    m_scaleDenom = denom;
                    if (getenv("AGB_IMG_SCALE_DEBUG"))
                        fprintf(stderr, "[img-scale] jpeg natural=%dx%d display=%dx%d denom=%d out=%ux%u\n",
                                (int)m_info.image_width, (int)m_info.image_height,
                                m_decoder->scaleHint().width(), m_decoder->scaleHint().height(),
                                denom, (unsigned)m_info.output_width, (unsigned)m_info.output_height);
                }

                /* Start decompressor */
                if (!jpeg_start_decompress(&m_info))
                    return true; /* I/O suspension */

                /* If this is a progressive JPEG ... */
                m_state = (m_info.buffered_image) ? JPEG_DECOMPRESS_PROGRESSIVE : JPEG_DECOMPRESS_SEQUENTIAL;
            }
    
            case JPEG_DECOMPRESS_SEQUENTIAL:
            {
                if (m_state == JPEG_DECOMPRESS_SEQUENTIAL) {
      
                    if (!m_decoder->outputScanlines())
                        return true; /* I/O suspension */

                    /* Budget ran out mid-image: stay in this state, the next
                     * call resumes the scanline loop (hasMoreWork is true). */
                    if (m_decoder->hasMoreWork())
                        return true;

                    /* If we've completed image output ... */
                    assert(m_info.output_scanline == m_info.output_height);
                    m_state = JPEG_DONE;
                }
            }

            case JPEG_DECOMPRESS_PROGRESSIVE:
            {
                if (m_state == JPEG_DECOMPRESS_PROGRESSIVE) {
                    int status;
                    do {
                        status = jpeg_consume_input(&m_info);
                    } while ((status != JPEG_SUSPENDED) &&
                             (status != JPEG_REACHED_EOI));

                    for (;;) {
                        if (m_info.output_scanline == 0) {
                            int scan = m_info.input_scan_number;

                            /* if we haven't displayed anything yet (output_scan_number==0)
                            and we have enough data for a complete scan, force output
                            of the last full scan */
                            if ((m_info.output_scan_number == 0) &&
                                (scan > 1) &&
                                (status != JPEG_REACHED_EOI))
                                scan--;

                            if (!jpeg_start_output(&m_info, scan))
                                return true; /* I/O suspension */
                        }

                        if (m_info.output_scanline == 0xffffff)
                            m_info.output_scanline = 0;

                        if (!m_decoder->outputScanlines()) {
                            if (m_info.output_scanline == 0)
                                /* didn't manage to read any lines - flag so we don't call
                                jpeg_start_output() multiple times for the same scan */
                                m_info.output_scanline = 0xffffff;
                            return true; /* I/O suspension */
                        }

                        /* Same as the sequential path: finish this scan on the
                         * next call instead of blowing the paint budget. */
                        if (m_decoder->hasMoreWork())
                            return true;

                        if (m_info.output_scanline == m_info.output_height) {
                            if (!jpeg_finish_output(&m_info))
                                return true; /* I/O suspension */

                            if (jpeg_input_complete(&m_info) &&
                                (m_info.input_scan_number == m_info.output_scan_number))
                                break;

                            m_info.output_scanline = 0;
                        }
                    }

                    m_state = JPEG_DONE;
                }
            }

            case JPEG_DONE:
            {
                /* Finish decompression */
                if (!jpeg_finish_decompress(&m_info))
                    return true; /* I/O suspension */

                m_state = JPEG_SINK_NON_JPEG_TRAILER;

                /* we're done */
                break;
            }
            
            case JPEG_SINK_NON_JPEG_TRAILER:
                break;

            case JPEG_ERROR:
                break;
        }

        return true;
    }

    jpeg_decompress_struct* info() { return &m_info; }
    JSAMPARRAY samples() const { return m_samples; }
    JPEGImageDecoder* decoder() { return m_decoder; }

private:
    JPEGImageDecoder* m_decoder;
    unsigned m_bufferLength;
    int m_bytesToSkip;
    bool m_decodingSizeOnly;
    bool m_initialized;
    int m_scaleDenom; // libjpeg DCT scale actually applied (1/1, 1/2, 1/4, 1/8)

    jpeg_decompress_struct m_info;
    decoder_error_mgr m_err;
    jstate m_state;

    JSAMPARRAY m_samples;
};

/* Override the standard error method in the IJG JPEG decoder code. */
void error_exit(j_common_ptr cinfo)
{
    /* Return control to the setjmp point. */
    decoder_error_mgr *err = (decoder_error_mgr *) cinfo->err;
    longjmp(err->setjmp_buffer, -1);
}

void init_source(j_decompress_ptr jd)
{
}

void skip_input_data(j_decompress_ptr jd, long num_bytes)
{
    decoder_source_mgr *src = (decoder_source_mgr *)jd->src;
    src->decoder->skipBytes(num_bytes);
}

boolean fill_input_buffer(j_decompress_ptr jd)
{
    // Our decode step always sets things up properly, so if this method is ever
    // called, then we have hit the end of the buffer.  A return value of FALSE indicates
    // that we have no data to supply yet.
    return false;
}

void term_source (j_decompress_ptr jd)
{
    decoder_source_mgr *src = (decoder_source_mgr *)jd->src;
    src->decoder->decoder()->jpegComplete();
}

JPEGImageDecoder::JPEGImageDecoder()
: m_reader(0)
{

}

JPEGImageDecoder::~JPEGImageDecoder()
{
    delete m_reader;
}

/* L3: the image is drawn at m_scaleHint device pixels, so a big JPEG does not
 * need to be decoded at its natural size. libjpeg can scale by 1/1, 1/2, 1/4 or
 * 1/8 during the DCT step; pick the strongest one whose output still covers the
 * display (never decode smaller than what is shown, except when the image is
 * drawn larger than natural - then 1/1 is the only choice). */
void JPEGImageDecoder::setScaleHint(const IntSize& display)
{
    m_scaleHint = display;
}

int JPEGImageDecoder::scaleDenomFor(const IntSize& natural) const
{
    if (natural.width() <= 0 || natural.height() <= 0)
        return 1;
    if (m_scaleHint.width() <= 0 || m_scaleHint.height() <= 0)
        return 1;

    for (int denom = 8; denom >= 2; denom /= 2) {
        if ((natural.width() + denom - 1) / denom >= m_scaleHint.width()
            && (natural.height() + denom - 1) / denom >= m_scaleHint.height())
            return denom;
    }
    return 1;
}

// Take the data and store it.
void JPEGImageDecoder::setData(const Vector<char>& data, bool allDataReceived)
{
    if (m_failed)
        return;

    // Cache our new data.
    ImageDecoder::setData(data, allDataReceived);

    // Create the JPEG reader.
    if (!m_reader && !m_failed)
        m_reader = new JPEGImageReader(this);
}

// Whether or not the size information has been decoded yet.
bool JPEGImageDecoder::isSizeAvailable() 
{
    // If we have pending data to decode, send it to the JPEG reader now.
    if (!m_sizeAvailable && m_reader) {
        if (m_failed)
            return false;

        // The decoder will go ahead and aggressively consume everything up until the
        // size is encountered.
        decode(true);
    }

    return m_sizeAvailable;
}

PixelsBuffer* JPEGImageDecoder::frameBufferAtIndex(size_t index)
{
    if (index)
        return 0;

    if (m_frameBufferCache.isEmpty())
        m_frameBufferCache.resize(1);

    PixelsBuffer& frame = m_frameBufferCache[0];
    if (frame.status() != PixelsBuffer::FrameComplete && m_reader)
        // Decode this frame.
        decode();

    return &frame;
}

// Feed data to the JPEG reader.
void JPEGImageDecoder::decode(bool sizeOnly)
{
    if (m_failed)
        return;

    m_failed = !m_reader->decode(m_data, sizeOnly);

    if (m_failed || (!m_frameBufferCache.isEmpty() && m_frameBufferCache[0].status() == PixelsBuffer::FrameComplete)) {
        delete m_reader;
        m_reader = 0;
        m_data.clear();
    }
}

bool JPEGImageDecoder::outputScanlines()
{
    if (m_frameBufferCache.isEmpty() || !m_reader)
        return false;

    jpeg_decompress_struct* info = m_reader->info();

    /* The buffer holds the *output* size, which is smaller than the image's
     * natural size when a DCT scale is in effect (L3). */
    const unsigned outputWidth = info->output_width;
    const unsigned outputHeight = info->output_height;

    PixelsBuffer& buffer = m_frameBufferCache[0];
    if (buffer.status() == PixelsBuffer::FrameEmpty) {
        // Let's resize our buffer now to the correct width/height.
        PixelArray& bytes = buffer.bytes();
        bytes.resize(outputWidth * outputHeight);
        bytes.init(0x0);
        // Update our status to be partially complete.
        buffer.setStatus(PixelsBuffer::FramePartial);
        buffer.setFrameSize(outputWidth, outputHeight);

        // For JPEGs, the frame always fills the entire image.
        buffer.setRect(IntRect(0, 0, m_size.width(), m_size.height()));

        // We don't have alpha (this is the default when the buffer is constructed).
    }

    JSAMPARRAY samples = m_reader->samples();

    unsigned dst = 0;  
    /* Bounded row loop: paint calls us from inside its traversal, so a whole
     * 12MP image (hundreds of ms of libjpeg) must not be decoded in one go.
     * When the budget runs out we return with the frame still FramePartial and
     * hasMoreWork() true; the next call resumes at output_scanline. */
    int rowsDone = 0;
    while (info->output_scanline < outputHeight) {
        if (budgetExhausted(rowsDone)) {
            setMoreWork(true);
            return true;
        }
        /* Request one scanline.  Returns 0 or 1 scanlines. */
        unsigned int curr_sl = info->output_scanline * outputWidth;
        if (jpeg_read_scanlines(info, samples, 1) != 1) {
            /* Out of input bytes: not "more work now", so the image does not
             * ask for a repaint until the next data arrival does. */
            setMoreWork(false);
            return false;
        }
        JSAMPLE *j1 = samples[0];
        for (unsigned i = 0; i < outputWidth; ++i) {
            unsigned r = *j1++;
            unsigned g = *j1++;
            unsigned b = *j1++;
            PixelsBuffer::setRGBA(dst, r, g, b, 0xFF);
            buffer.bytes().setPixel(i + curr_sl, dst);
        }

        buffer.ensureHeight(info->output_scanline);
        rowsDone++;
    }

    setMoreWork(false);
    return true;
}

void JPEGImageDecoder::jpegComplete()
{
    if (m_frameBufferCache.isEmpty())
        return;
	//NOTE: jpeg can not delete reader here because it has abort function call.
    // Hand back an appropriately sized buffer, even if the image ended up being empty.
    PixelsBuffer& buffer = m_frameBufferCache[0];
    buffer.setStatus(PixelsBuffer::FrameComplete);
    setMoreWork(false);
    /* term_source() runs inside the reader's decode(): m_data is what libjpeg
     * is reading from, so it may only be cleared by decode() after the call
     * returns (R62). */
}

}
#endif /* JPEG_SUPPORT */

