/* AgwViewProtocol.h - render-backend protocol v1 (docs/design-render-backend.md §4).
 *
 * One command in, one event out. The local backend maps a command back onto
 * macross_*; a process or RVVM backend serializes the same struct across its
 * ring. Fixed-layout POD on purpose: a serializer can copy it field by field
 * without a schema.
 *
 * `text` points into the submitter's memory and is only valid for the duration
 * of submit()/submitBatch(): a backend that crosses a boundary copies it into
 * its own ring. That is what keeps the hot path free of allocation.
 */
#ifndef AGW_VIEW_PROTOCOL_H
#define AGW_VIEW_PROTOCOL_H

#include <stdint.h>

#include "macross.h"

#define AGW_VIEW_PROTOCOL_VERSION 1

/* host -> view */
typedef enum {
    AGW_VIEW_CMD_NONE = 0,
    AGW_VIEW_CMD_VISIBLE,       /* a = 0/1 */
    AGW_VIEW_CMD_FOCUS,         /* a = 0/1 */
    AGW_VIEW_CMD_MOUSE,         /* a = MC_MOUSE_EVENT_TYPE, b = MC_MOUSE_BUTTON,
                                 * c = x, d = y */
    AGW_VIEW_CMD_KEY,           /* a = MC_KEY_EVENT_TYPE, b = modifier, c = vk */
    AGW_VIEW_CMD_TEXT,          /* text = UTF-8 commit (IME commit goes here) */
    AGW_VIEW_CMD_PREEDIT,       /* text = preedit, a = caret (reserved) */
    AGW_VIEW_CMD_NAVIGATE,      /* text = url */
    AGW_VIEW_CMD_LOAD_HTML,     /* text = html */
    AGW_VIEW_CMD_RELOAD,
    AGW_VIEW_CMD_STOP,
    AGW_VIEW_CMD_BACK,
    AGW_VIEW_CMD_FORWARD,
    AGW_VIEW_CMD_EVAL,          /* text = script */
    AGW_VIEW_CMD_ZOOM,          /* a = percent */
    AGW_VIEW_CMD_SEARCH,        /* text = needle, a = forward, b = caseSensitive */
    AGW_VIEW_CMD_CONTEXT_MENU,  /* a = modifier, c = x, d = y */
    AGW_VIEW_CMD_SERVICE_REPLY, /* a = id, b = status, text = payload (B4) */
    AGW_VIEW_CMD_EDITOR         /* text = Editor::execCommand name, e.g. "Copy" */
} AgwViewCommandKind;

typedef struct {
    int32_t     kind;       /* AgwViewCommandKind */
    int32_t     a, b, c, d; /* kind-specific scalars */
    int64_t     timeUs;     /* input timestamp (0 = not an input) */
    const char* text;       /* borrowed for the duration of the submit call */
    int32_t     textLen;
    /* Document the command was issued against (macross_view_document_epoch at
     * queue time; 0 = not stamped). A document-scoped command whose epoch no
     * longer matches runs against a page it was never meant for, so the view
     * drops it and answers AGW_VIEW_EVENT_COMMAND_DROPPED instead of either
     * mis-running it or silently doing nothing. */
    uint32_t    docEpoch;
} AgwViewCommand;

/* view -> host */
typedef enum {
    AGW_VIEW_EVENT_NONE = 0,
    AGW_VIEW_EVENT_READY,           /* a = protocol version, b = capabilities */
    AGW_VIEW_EVENT_FRAME,           /* frameBufferId/frameSeq + damage */
    AGW_VIEW_EVENT_TITLE,           /* text */
    AGW_VIEW_EVENT_PROGRESS,        /* a = percent, b = finished */
    AGW_VIEW_EVENT_URL,             /* text */
    AGW_VIEW_EVENT_STATUS,          /* text */
    AGW_VIEW_EVENT_MESSAGE,         /* text: agavePostMessage() reached the host */
    AGW_VIEW_EVENT_CONTEXT_MENU,    /* a,b = x,y; c,d = link/image byte lengths;
                                     * text = link + image + selection text. The
                                     * host answers by building the menu (the
                                     * engine cannot: no windows in this process) */
    AGW_VIEW_EVENT_DOCUMENT,        /* a = new document epoch (a document committed) */
    AGW_VIEW_EVENT_SERVICE_REQUEST, /* a = service kind, text = args (B4) */
    AGW_VIEW_EVENT_UNRESPONSIVE,    /* a = busy ms */
    AGW_VIEW_EVENT_COMMAND_DROPPED, /* a = AgwViewCommandKind, b = expected epoch,
                                     * c = current epoch, text = reason */
    AGW_VIEW_EVENT_EXIT,            /* a = exit code */
    /* Host callbacks the renderer cannot perform itself (no windows / no host
     * state in that process). One-way: the host applies the effect. */
    AGW_VIEW_EVENT_CLIPBOARD,       /* text = UTF-8, b = 1 when it was too large
                                     * to carry (a = its byte length, text empty) */
    AGW_VIEW_EVENT_CURSOR,          /* a = MC_CURSOR */
    AGW_VIEW_EVENT_HISTORY          /* a = back count, b = forward count */
} AgwViewEventKind;

/* Service kinds carried by AGW_VIEW_EVENT_SERVICE_REQUEST. v1 only needs the
 * window lifecycle: a backend that cannot create a window itself (it lives in
 * another process) asks the control, which owns the host-facing window API. */
typedef enum {
    AGW_VIEW_SERVICE_NONE = 0,
    AGW_VIEW_SERVICE_OPEN_WINDOW = 1,  /* text = "name\nurl", b = MC window flags */
    AGW_VIEW_SERVICE_CLOSE_WINDOW = 2  /* the view's page called window.close() */
} AgwViewServiceKind;

/* Capability bits reported by AGW_VIEW_EVENT_READY. */
#define AGW_VIEW_CAP_DAMAGE   0x1  /* host understands damage rectangles */
#define AGW_VIEW_CAP_GL       0x2  /* host may composite through GL */
#define AGW_VIEW_CAP_SERVICES 0x4  /* service request/reply channel (B4) */

typedef struct {
    int32_t     kind;   /* AgwViewEventKind */
    int32_t     a, b, c, d;
    int64_t     timeUs;
    uint32_t    frameBufferId;
    uint32_t    frameSeq;
    int32_t     damage[4]; /* x, y, w, h; empty = whole surface */
    const char* text;      /* borrowed: valid for the poll call only */
    int32_t     textLen;
    /* The document epoch the event was produced in, so the host can stamp
     * commands queued afterwards with the epoch of the page it is looking at. */
    uint32_t    docEpoch;
} AgwViewEvent;

/* The frame backpack: the view renders into `bits` (BGRA, `pitch` bytes per
 * row) and publishes it with AGW_VIEW_EVENT_FRAME; the host composites the
 * damage and answers with one ack, after which the view may reuse the buffer. */
typedef struct {
    uint32_t id;
    int32_t  format;   /* MC_PIXEL_FORMAT */
    int32_t  width, height, pitch;
    MC_BYTE* bits;
} AgwFrameBuffer;

typedef struct {
    uint32_t bufferId;
    uint32_t seq;
} AgwFrameAck;

#endif /* AGW_VIEW_PROTOCOL_H */
