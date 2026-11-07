/*
 * Copyright (C) 2024 Zhang Ji Peng (onecoolx@gmail.com)
 *
 * Container header parsing for HTMLMediaElement (see MediaMetadata.h and
 * docs/arch-media.md). Nothing here decodes samples: it walks the boxes /
 * EBML elements that carry duration and pixel size and stops.
 */

#include "config.h"
#include "MediaMetadata.h"

namespace WebCore {

namespace {

inline unsigned readU32BE(const unsigned char* p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

inline float readFloatBE(const unsigned char* p)
{
    unsigned bits = readU32BE(p);
    float f;

    memcpy(&f, &bits, sizeof(f));
    return f;
}

/* Duration in current WebM files is a float64 (EBML float, 8 bytes); the
 * older 4 byte float form is still accepted by the caller. */
inline double readDoubleBE(const unsigned char* p)
{
    unsigned long long bits = ((unsigned long long)readU32BE(p) << 32) | readU32BE(p + 4);
    double d;

    memcpy(&d, &bits, sizeof(d));
    return d;
}

inline double readU32Fixed16_16(const unsigned char* p)
{
    /* 16.16 fixed point: the integer part is what browsers report. */
    return (double)readU32BE(p) / 65536.0;
}

/* ---- MP4 / ISO BMFF ---------------------------------------------------- */

struct Mp4State {
    double duration;
    int width;
    int height;
    bool hasVideo;
    bool hasAudio;
};

void walkMp4(const unsigned char* p, size_t size, Mp4State& st, int depth)
{
    size_t offset = 0;

    if (depth > 8)
        return;

    while (offset + 8 <= size) {
        unsigned long long boxSize = readU32BE(p + offset);
        const unsigned char* type = p + offset + 4;
        size_t headerSize = 8;

        if (boxSize == 1) {
            if (offset + 16 > size)
                return;
            boxSize = ((unsigned long long)readU32BE(p + offset + 8) << 32) | readU32BE(p + offset + 12);
            headerSize = 16;
        } else if (boxSize == 0) {
            boxSize = size - offset;
        }

        if (boxSize < headerSize || offset + boxSize > size)
            return;

        const unsigned char* body = p + offset + headerSize;
        size_t bodySize = (size_t)(boxSize - headerSize);

        if (!memcmp(type, "moov", 4) || !memcmp(type, "trak", 4) || !memcmp(type, "mdia", 4)) {
            walkMp4(body, bodySize, st, depth + 1);
        } else if (!memcmp(type, "mvhd", 4) && bodySize >= 20) {
            unsigned version = body[0];
            if (version == 1 && bodySize >= 32) {
                unsigned timescale = readU32BE(body + 20);
                unsigned long long duration =
                    ((unsigned long long)readU32BE(body + 24) << 32) | readU32BE(body + 28);
                if (timescale)
                    st.duration = (double)duration / (double)timescale;
            } else {
                unsigned timescale = readU32BE(body + 12);
                unsigned duration = readU32BE(body + 16);
                if (timescale)
                    st.duration = (double)duration / (double)timescale;
            }
        } else if (!memcmp(type, "tkhd", 4) && bodySize >= 84) {
            /* width/height are the last two 16.16 values of tkhd. */
            double w = readU32Fixed16_16(body + bodySize - 8);
            double h = readU32Fixed16_16(body + bodySize - 4);
            if (w > 0 && h > 0) {
                st.width = (int)(w + 0.5);
                st.height = (int)(h + 0.5);
                st.hasVideo = true;
            }
        } else if (!memcmp(type, "hdlr", 4) && bodySize >= 12) {
            const unsigned char* handler = body + 8;
            if (!memcmp(handler, "vide", 4))
                st.hasVideo = true;
            else if (!memcmp(handler, "soun", 4))
                st.hasAudio = true;
        } else if (!memcmp(type, "stsd", 4)) {
            /* Not required for size/duration; skip. */
        }

        offset += (size_t)boxSize;
    }
}

/* ---- WebM / Matroska (EBML) ------------------------------------------- */

struct EbmlState {
    double durationMs;
    double timecodeScale;   /* ns per timecode unit, default 1000000 */
    int width;
    int height;
    bool hasVideo;
    bool hasAudio;
};

/* Returns the number of bytes of the vint, and the value with the marker
 * stripped. `allOnes` reports the "unknown size" encoding. */
size_t readVint(const unsigned char* p, size_t size, unsigned long long& value, bool& unknown)
{
    unsigned char first;
    size_t length = 0;
    unsigned char mask = 0x80;

    unknown = false;

    if (!size)
        return 0;

    first = p[0];
    while (length < 8 && !(first & mask)) {
        mask >>= 1;
        ++length;
    }
    if (length >= 8)
        return 0;
    ++length;
    if (length > size)
        return 0;

    value = first & (mask - 1);
    for (size_t i = 1; i < length; ++i)
        value = (value << 8) | p[i];

    /* All data bits set means "unknown size" in an element header. */
    unknown = (value == ((1ULL << (7 * (length - 1))) - 1));
    return length;
}

unsigned long long readUnsignedBE(const unsigned char* p, size_t size)
{
    unsigned long long v = 0;
    for (size_t i = 0; i < size; ++i)
        v = (v << 8) | p[i];
    return v;
}

void walkEbml(const unsigned char* p, size_t size, EbmlState& st, int depth);

void walkEbmlChildren(const unsigned char* p, size_t size, EbmlState& st, int depth)
{
    size_t offset = 0;

    if (depth > 8)
        return;

    while (offset < size) {
        unsigned long long id = 0;
        unsigned long long length = 0;
        bool unknown = false;
        size_t idLen = readVint(p + offset, size - offset, id, unknown);
        size_t lenLen;
        const unsigned char* body;
        size_t bodySize;

        if (!idLen || offset + idLen > size)
            return;

        /* IDs keep their marker bits; read them raw (they are canonical). */
        id = readUnsignedBE(p + offset, idLen);
        lenLen = readVint(p + offset + idLen, size - offset - idLen, length, unknown);
        if (!lenLen)
            return;

        body = p + offset + idLen + lenLen;
        bodySize = unknown ? (size - (offset + idLen + lenLen)) : (size_t)length;
        if (body + bodySize > p + size)
            bodySize = size - (size_t)(body - p);

        /* Master elements: Segment / Info / Tracks / TrackEntry / Video. Info
         * carries Duration and TimecodeScale, so missing it is why a WebM
         * reported 320x240 but a duration of 0. */
        if (id == 0x18538067ULL      /* Segment */
                || id == 0x1549A966ULL /* Info */
                || id == 0x1654AE6BULL /* Tracks */
                || id == 0xAEULL       /* TrackEntry */
                || id == 0xE0ULL) {    /* Video */
            walkEbml(body, bodySize, st, depth + 1);
        } else if (id == 0x4489ULL) {
            /* Duration: a float, 4 bytes in old muxers, 8 in current ones. */
            if (bodySize >= 8)
                st.durationMs = readDoubleBE(body);
            else if (bodySize >= 4)
                st.durationMs = (double)readFloatBE(body);
        } else if (id == 0x2AD7B1ULL) {
            st.timecodeScale = (double)readUnsignedBE(body, bodySize);
        } else if (id == 0xB0ULL) {
            st.width = (int)readUnsignedBE(body, bodySize);
        } else if (id == 0xBAULL) {
            st.height = (int)readUnsignedBE(body, bodySize);
        } else if (id == 0x83ULL) {
            if (bodySize >= 1) {
                if (body[0] == 1)
                    st.hasVideo = true;
                else if (body[0] == 2)
                    st.hasAudio = true;
            }
        }

        offset += idLen + lenLen + bodySize;
    }
}

void walkEbml(const unsigned char* p, size_t size, EbmlState& st, int depth)
{
    walkEbmlChildren(p, size, st, depth);
}

/* ---- Ogg --------------------------------------------------------------- */

/* Ogg keeps the duration in the last page's granule position. Reading it needs
 * the tail, so the header pass only detects the container and fills a codec
 * level mime guess; parseMediaTail() completes the duration. */
bool isOgg(const unsigned char* p, size_t size)
{
    return size >= 4 && !memcmp(p, "OggS", 4);
}

} // namespace

String mediaMimeTypeForURL(const String& url)
{
    String lower = url.lower();
    int dot = lower.reverseFind('.');

    if (dot < 0)
        return String();

    String ext = lower.substring(dot + 1);
    int query = ext.find('?');
    if (query >= 0)
        ext = ext.substring(0, query);

    if (ext == "mp4" || ext == "m4v" || ext == "m4a")
        return "video/mp4";
    if (ext == "webm")
        return "video/webm";
    if (ext == "ogv")
        return "video/ogg";
    if (ext == "ogg" || ext == "oga" || ext == "opus")
        return "audio/ogg";
    if (ext == "mp3")
        return "audio/mpeg";
    if (ext == "wav")
        return "audio/wav";
    if (ext == "m3u8")
        return "application/x-mpegURL";
    if (ext == "ts")
        return "video/mp2t";
    if (ext == "mkv")
        return "video/x-matroska";
    if (ext == "flac")
        return "audio/flac";

    return String();
}

bool parseMediaHeader(const unsigned char* data, size_t size, MediaInfo& out)
{
    if (!data || size < 12)
        return false;

    /* MP4/MOV: the first box is almost always ftyp (or a wide/free box). */
    if (!memcmp(data + 4, "ftyp", 4)) {
        Mp4State st;

        st.duration = 0;
        st.width = st.height = 0;
        st.hasVideo = st.hasAudio = false;

        walkMp4(data, size, st, 0);

        out.ok = true;
        out.duration = st.duration;
        out.width = st.width;
        out.height = st.height;
        out.hasVideo = st.hasVideo || st.height > 0;
        out.hasAudio = st.hasAudio;
        out.mimeType = "video/mp4";
        return true;
    }

    /* Matroska/WebM: EBML magic. */
    if (size >= 4 && data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF && data[3] == 0xA3) {
        EbmlState st;

        st.durationMs = 0;
        st.timecodeScale = 1000000.0;
        st.width = st.height = 0;
        st.hasVideo = st.hasAudio = false;

        walkEbml(data, size, st, 0);

        out.ok = true;
        out.duration = st.durationMs * st.timecodeScale / 1000000000.0;
        out.width = st.width;
        out.height = st.height;
        out.hasVideo = st.hasVideo;
        out.hasAudio = st.hasAudio;
        out.mimeType = "video/webm";
        return true;
    }

    if (isOgg(data, size)) {
        out.ok = true;
        out.duration = 0;
        out.width = 0;
        out.height = 0;
        out.hasVideo = false;
        out.hasAudio = true;
        out.mimeType = "audio/ogg";
        return true;
    }

    return false;
}

bool parseMediaTail(const unsigned char* data, size_t size, MediaInfo& out)
{
    /* Ogg granule position: the last "OggS" page header carries a 64 bit
     * granule (sample count) at offset 6, and the codec's rate is not in the
     * tail - so this only recovers a *duration in granules*, which is not a
     * duration in seconds. Leave it to ffprobe instead of guessing. */
    (void)data;
    (void)size;
    (void)out;
    return false;
}

} // namespace WebCore
