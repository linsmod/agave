/* AgwViewIpc.h - wire format of the process backend (S3 B2).
 *
 * One named shared mapping per view holds the canvas, a command ring, an event
 * ring and two text arenas. Indices are single-producer/single-consumer longs:
 * the control writes commands, the renderer reads them; the renderer writes
 * events and the canvas, the control reads them. No locks.
 *
 * Pointer fields of AgwViewCommand/AgwViewEvent are meaningless across the
 * boundary, so the wire structs carry text offsets into the arenas instead.
 */
#ifndef AGW_VIEW_IPC_H
#define AGW_VIEW_IPC_H

#include <stdint.h>
#include <string.h>

#include "AgwViewProtocol.h"

#define AGW_IPC_MAGIC      0x41564950u /* 'AVIP' */
#define AGW_IPC_VERSION    2

#define AGW_IPC_CMD_SLOTS  64
#define AGW_IPC_EVT_SLOTS  64
/* Text arena. A command's text is copied in whole; larger payloads use a
 * one-shot section instead (AGW_IPC_CMD_PAYLOAD) - the arena is a small,
 * always-mapped ring, not a capacity for arbitrary scripts. */
#define AGW_IPC_TEXT_BYTES 65536
#define AGW_IPC_CRASH_BYTES 24576

/* Host policy: texts up to this size travel through the text arena, anything
 * larger through a payload section (one CreateFileMapping of exactly the
 * payload's size). Not a hard wire limit: the renderer accepts any contiguous
 * arena text up to AGW_IPC_TEXT_BYTES. */
#define AGW_IPC_INLINE_TEXT 16384
/* Largest text one event can carry (pushEvent caps there; the control keeps one
 * buffer of this size per event of a poll batch). Clipboard copies are the
 * reason it is not smaller. */
#define AGW_IPC_EVENT_TEXT_MAX (AGW_IPC_TEXT_BYTES / 2)
/* Sanity cap for a payload section: a host that asks for more than this is
 * refused instead of asked to allocate it. */
#define AGW_IPC_PAYLOAD_MAX (64 * 1024 * 1024)
#define AGW_IPC_PAYLOAD_NAME 64

/* Text arena protocol (single producer = control, single consumer = renderer).
 *
 *   cmdTextWrite  producer's next offset, 0..AGW_IPC_TEXT_BYTES
 *   cmdTextRead   end offset of the last command whose text the renderer COPIED
 *                 out, 0..AGW_IPC_TEXT_BYTES
 *
 * A payload is always contiguous. If it does not fit before the end, the
 * producer pads the tail (the write counter jumps to 0) and the consumer acks
 * the new payload's end; (write - read) then still counts the padding as used
 * until that command is consumed. The producer may only write when
 * (padding + length) + (write - read) <= AGW_IPC_TEXT_BYTES, otherwise it waits
 * for the renderer's `_txt` event (signalled after every command is consumed).
 * The consumer acks the text *before* executing the command, so a long script
 * cannot stall the arena. No silent truncation on either side. */

/* The canvas is allocated for this size once at create, and a resize only
 * changes how much of it is used; a stale full-size canvas avoids recreating
 * the mapping (and the renderer's view) on every window drag. 16:10 covers the
 * shell's 1100x800 viewport and most maximized windows; larger crops at the
 * edge (B2 keeps size fixed; the mapping grows in B2b). */
#define AGW_IPC_FRAME_W_MAX 2560
#define AGW_IPC_FRAME_H_MAX 1600

/* Wire-only commands (the protocol itself has no buffer-carrying ops). */
#define AGW_IPC_CMD_EXIT   1000
#define AGW_IPC_CMD_RESIZE 1001   /* a = width, b = height */
/* A command whose text was too large for the inline arena. `payloadName` is a
 * named shared section the control filled with the payload before posting:
 *   a = the kind of the command to execute with that payload
 *       (EVAL / LOAD_HTML / NAVIGATE - kinds whose scalars are unused)
 *   b = payload byte count
 *   payloadName = "Local\\agave_pay_<pid>_<counter>"
 * The renderer maps the section, copies the bytes (the section is only
 * guaranteed to live until the renderer acks `_pay`), then executes. */
#define AGW_IPC_CMD_PAYLOAD 1002

typedef struct {
    int32_t kind;
    int32_t a, b, c, d;
    int64_t timeUs;
    int32_t textOffset;   /* into cmdText */
    int32_t textLen;
    uint32_t docEpoch;    /* document the command was issued against */
    char    payloadName[AGW_IPC_PAYLOAD_NAME]; /* AGW_IPC_CMD_PAYLOAD only */
} AgwIpcCommand;

typedef struct {
    int32_t kind;
    int32_t a, b, c, d;
    int64_t timeUs;
    uint32_t frameBufferId;
    uint32_t frameSeq;
    int32_t damage[4];
    int32_t textOffset;   /* into evtText */
    int32_t textLen;
    uint32_t docEpoch;    /* document the event was produced in */
} AgwIpcEvent;

typedef struct {
    uint32_t magic;
    uint32_t version;

    /* What the control last asked for. */
    volatile LONG width;
    volatile LONG height;

    /* Command ring (control -> renderer). */
    volatile LONG commandWrite;
    volatile LONG commandRead;
    volatile LONG cmdTextWrite;
    volatile LONG cmdTextRead;
    AgwIpcCommand commands[AGW_IPC_CMD_SLOTS];
    char cmdText[AGW_IPC_TEXT_BYTES];

    /* Event ring (renderer -> control). */
    volatile LONG eventWrite;
    volatile LONG eventRead;
    volatile LONG evtTextWrite;
    volatile LONG evtTextRead;
    AgwIpcEvent events[AGW_IPC_EVT_SLOTS];
    char evtText[AGW_IPC_TEXT_BYTES];

    /* Canvas: BGRA, pitch = framePitch bytes/row. The renderer paints into it
     * and bumps frameSeq; the control copies it out and answers ackSeq. */
    volatile LONG frameSeq;
    volatile LONG ackSeq;
    volatile LONG visible;
    volatile LONG running;
    volatile LONG exitCode;
    uint32_t frameOffset;   /* from the start of this struct */
    uint32_t framePitch;
    uint32_t frameBytes;

    /* Renderer crash report (environment + symbolized stack), written once by
     * the renderer's last-chance handler. The section outlives the process, so
     * the control can read the text after the renderer is gone and put it on
     * the "Page crashed" screen. crashLen bytes used, 0 = no report. */
    volatile LONG crashLen;
    char crashText[AGW_IPC_CRASH_BYTES];
} AgwIpcHeader;

static inline char* agwIpcText(AgwIpcHeader* hdr, int32_t offset)
{
    if (offset < 0 || offset >= AGW_IPC_TEXT_BYTES)
        return NULL;
    return &hdr->cmdText[offset];
}

static inline char* agwIpcEvtText(AgwIpcHeader* hdr, int32_t offset)
{
    if (offset < 0 || offset >= AGW_IPC_TEXT_BYTES)
        return NULL;
    return &hdr->evtText[offset];
}

static inline uint8_t* agwIpcFrame(AgwIpcHeader* hdr)
{
    return (uint8_t*)hdr + hdr->frameOffset;
}

#endif /* AGW_VIEW_IPC_H */
