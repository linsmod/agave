/*
 * DownloadEvents.h - the one hook between the download manager and the JS layer.
 *
 * The internal page (agave://downloads) has to repaint when a job changes, and
 * the change happens on the engine thread inside DownloadJob::emit(). The ant
 * runtime owns the realm, so it installs the sink and decides whether the
 * running document is allowed to see it (only internal pages are).
 *
 * A plain function pointer keeps the webview layer free of a link dependency on
 * the JS backend.
 */

#ifndef DownloadEvents_h
#define DownloadEvents_h

typedef void (*AgbDownloadEventSink)(int id, int state);

extern AgbDownloadEventSink g_agbDownloadEventSink;

#endif /* DownloadEvents_h */
