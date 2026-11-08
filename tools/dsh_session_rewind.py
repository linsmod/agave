#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dsh_session_rewind.py - undo / redo over a DSH session log (session.v4.jsonl.zstd).

The session log is an append-only event stream: the session header first, then
events carrying a contiguous `seq`.  A session is therefore fully described by a
*prefix* of that stream.  This tool keeps a pristine copy (A-backup), moves a
cursor over A-backup, and regenerates the real log A as `prefix(A-backup,
cursor)` - so `/undo` `/redo` walk the content in either direction and A can be
cut anywhere between A-start (header only) and A-pos (the whole backup).

    # where am I, what turns exist, where did the compactions happen
    python tools/dsh_session_rewind.py --session <id> status
    python tools/dsh_session_rewind.py --session <id> list

    # one turn back / forward (creates A-backup on first use)
    python tools/dsh_session_rewind.py --session <id> undo
    python tools/dsh_session_rewind.py --session <id> redo

    # arbitrary targets
    python tools/dsh_session_rewind.py --session <id> goto --turn 3
    python tools/dsh_session_rewind.py --session <id> goto --before-compaction
    python tools/dsh_session_rewind.py --session <id> goto --start
    python tools/dsh_session_rewind.py --session <id> restore        # = goto --end

    # recommended for the session DSH is currently showing: quit DSH, then
    python tools/dsh_session_rewind.py --session <id> goto --before-compaction
    python tools/dsh_session_rewind.py --session <id> retire-cache   # refold on open
    # ...and start DSH again.  The running window never reloads a session on
    # its own, and it rewrites the derived projection cache from memory, so a
    # walk done while it runs shows up only after the app is restarted.

Cursor semantics: a cursor is a *seq cutoff*; the log keeps every event with
`seq < cursor` (the seq-less session header is always kept).  `undo` goes to the
start of the previous turn, `redo` to the start of the next one; `restore` goes
to the end of the backup.

What it touches next to the session:

    <session>/session.v4.jsonl.zstd            the live log (rewritten)
    <session>/session.v4.jsonl.zstd.backup     A-backup, the pristine copy
    <session>/session.v4.jsonl.zstd.rewind.json  cursor state
    <session>/rewind/*.zstd                    one snapshot of A per write
    <projcache>/sessions/<id>.json.bak-<ts>    retired derived cache

The projection cache is a *derived* document stamped with a seq; truncating the
log would leave it ahead of the log, so each write retires it (the loader treats
a missing record as absent and refolds from the log).

Live sessions: DSH holds the session it is showing in memory.  Rewriting the log
of that session changes what the next load shows, not what the running window
shows, and the running window keeps writing (log appends, projection cache), so
`retire-cache` has to run with DSH closed.  Writes refuse while the log was
modified in the last --live-window seconds unless --force is given, and every
write snapshots the current A first.

zstd is required (Windows: C:\\msys64\\mingw64\\bin\\zstd.exe, or set DSH_ZSTD /
put zstd on PATH).
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

DEFAULT_SESSIONS_ROOT = os.path.join(os.path.expanduser("~"), ".dsh", "sessions")
DEFAULT_PROJCACHE_ROOT = os.path.join(
    os.path.expanduser("~"), ".dsh", "storages", "session_projcache")
LOG_NAME = "session.v4.jsonl.zstd"
BACKUP_SUFFIX = ".backup"
CURSOR_SUFFIX = ".rewind.json"
CURSOR_VERSION = 1
SNAPSHOT_DIR = "rewind"


# --------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------

def fail(message):
    sys.stderr.write("error: %s\n" % message)
    sys.exit(1)


def find_zstd():
    candidates = []
    env = os.environ.get("DSH_ZSTD")
    if env:
        candidates.append(env)
    found = shutil.which("zstd")
    if found:
        candidates.append(found)
    candidates += [
        r"C:\msys64\mingw64\bin\zstd.exe",
        r"C:\msys64\usr\bin\zstd.exe",
    ]
    for path in candidates:
        if path and os.path.isfile(path):
            return path
    fail("zstd not found; install it, set DSH_ZSTD, or put zstd on PATH")


ZSTD = None


def decompress(path):
    """Returns the decompressed bytes of a zstd file."""
    proc = subprocess.run([ZSTD, "-q", "-d", "-c", path],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        fail("zstd -d failed for %s:\n%s" % (path, proc.stderr.decode("utf-8", "replace")))
    return proc.stdout


def compress_to(src_path, dst_path):
    proc = subprocess.run([ZSTD, "-q", "-19", "-f", "-o", dst_path, src_path],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        fail("zstd -19 failed for %s:\n%s" % (dst_path, proc.stderr.decode("utf-8", "replace")))


def split_lines(payload):
    """Decompressed event stream -> list of raw lines, each keeping its '\\n'."""
    return payload.splitlines(keepends=True)


def event_meta(line):
    """(type, seq) of one JSONL event; (None, None) when the line is not JSON."""
    text = line.strip()
    if not text:
        return None, None
    try:
        obj = json.loads(text)
    except ValueError:
        return None, None
    seq = obj.get("seq")
    if not isinstance(seq, int):
        seq = None
    return obj.get("type"), seq


def load_events(path_or_bytes):
    """Parses a decompressed stream into [(line_bytes, type, seq, obj)]."""
    if isinstance(path_or_bytes, bytes):
        payload = path_or_bytes
    else:
        payload = decompress(path_or_bytes)
    events = []
    for raw in split_lines(payload):
        text = raw.strip()
        if not text:
            continue
        try:
            obj = json.loads(text)
        except ValueError:
            # A live log's last line can be a torn write: stop there, that is
            # the "上一轮为止" boundary the readers also use.
            break
        seq = obj.get("seq")
        events.append((raw if raw.endswith(b"\n") else raw + b"\n",
                       obj.get("type"), seq if isinstance(seq, int) else None, obj))
    return events


def u8(value):
    return value if value else ""


def first_text(obj):
    """First text block of a message event, single line, truncated."""
    data = obj.get("data") or {}
    content = data.get("content") or (data.get("message") or {}).get("content") or []
    for block in content:
        if isinstance(block, dict) and block.get("type") == "text":
            text = (block.get("text") or "").strip().replace("\r", "")
            text = " ".join(text.split())
            return text[:100]
    return ""


# --------------------------------------------------------------------------
# session discovery / paths
# --------------------------------------------------------------------------

def resolve_session(session_id, sessions_root):
    """Exact dir name, then unique prefix, then unique substring."""
    direct = os.path.join(sessions_root, "**", session_id, LOG_NAME)
    hits = glob.glob(direct, recursive=True)
    if not hits:
        pattern = os.path.join(sessions_root, "**", "*%s*" % session_id, LOG_NAME)
        hits = [h for h in glob.glob(pattern, recursive=True)
                if os.path.basename(os.path.dirname(h)) != session_id]
        if len(hits) > 1:
            names = sorted(os.path.basename(os.path.dirname(h)) for h in hits)
            fail("session id %r is ambiguous: %s" % (session_id, ", ".join(names)))
    if not hits:
        fail("no session matching %r under %s" % (session_id, sessions_root))
    return os.path.abspath(hits[0])


class SessionPaths(object):
    def __init__(self, log_path, projcache_root):
        self.log = log_path
        self.dir = os.path.dirname(log_path)
        self.id = os.path.basename(self.dir)
        self.backup = log_path + BACKUP_SUFFIX
        self.cursor = log_path + CURSOR_SUFFIX
        self.snapshots = os.path.join(self.dir, SNAPSHOT_DIR)
        self.projcache_record = None
        self.projcache_retired = []
        if projcache_root:
            record = os.path.join(projcache_root, "sessions", self.id + ".json")
            if os.path.isfile(record):
                self.projcache_record = record


# --------------------------------------------------------------------------
# backup / cursor state
# --------------------------------------------------------------------------

def read_cursor(paths):
    if not os.path.isfile(paths.cursor):
        return None
    try:
        with open(paths.cursor, "r", encoding="utf-8") as handle:
            state = json.load(handle)
    except (OSError, ValueError) as exc:
        fail("cannot read cursor state %s: %s" % (paths.cursor, exc))
    if state.get("version") != CURSOR_VERSION:
        fail("cursor state %s has unsupported version %r" % (paths.cursor, state.get("version")))
    return state


def write_cursor(paths, cutoff, total, note):
    state = {
        "version": CURSOR_VERSION,
        "backup": os.path.basename(paths.backup),
        "cursor": cutoff,
        "total": total,
        "updatedAt": int(time.time() * 1000),
        "note": note,
    }
    tmp = paths.cursor + ".tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(state, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    os.replace(tmp, paths.cursor)


def ensure_backup(paths, dry_run):
    """Creates A-backup from the current log; returns (backup_events, created)."""
    if os.path.isfile(paths.backup):
        return load_events(paths.backup), False
    if dry_run:
        # Read-only walk: pretend the log already is the backup.
        return load_events(paths.log), False
    # Copy the compressed bytes as they are: A-backup must be the log the user
    # started from, not a re-encoded version of it. The log is written in
    # zstd frames, so a copy taken while a frame is being appended can end in a
    # torn frame - verify it decompresses, retry once, then give up loudly
    # rather than keep a backup that cannot be read back.
    for attempt in (1, 2):
        shutil.copy2(paths.log, paths.backup)
        try:
            return load_events(paths.backup), True
        except SystemExit:
            if attempt == 2:
                raise
            time.sleep(0.2)
    return load_events(paths.backup), True


def backup_total(events):
    seqs = [seq for (_, _, seq, _) in events if seq is not None]
    return (max(seqs) + 1) if seqs else 0


# --------------------------------------------------------------------------
# turn / marker model over the backup
# --------------------------------------------------------------------------

class Turn(object):
    def __init__(self, number, start_seq):
        self.number = number
        self.start_seq = start_seq
        self.end_seq = None
        self.start_time = None
        self.first_user = ""

    def __repr__(self):
        return "turn %d [%s..%s]" % (self.number, self.start_seq, self.end_seq)


def build_model(events):
    turns = []
    compactions = []
    current = None
    for (_, etype, seq, obj) in events:
        data = obj.get("data") or {}
        if etype == "turn/start":
            current = Turn(data.get("turn"), seq)
            current.start_time = obj.get("time")
            turns.append(current)
        elif etype == "turn/end":
            if current is not None:
                current.end_seq = seq
            current = None
        elif etype == "user/message" and current is not None and not current.first_user:
            if (data.get("source") or {}).get("kind") == "user":
                current.first_user = first_text(obj)
        elif etype in ("compaction/start", "compaction/summary", "compaction/end"):
            compactions.append((etype, seq, obj))
    return turns, compactions


def undo_cursor(turns, cutoff, steps):
    for _ in range(steps):
        previous = [t.start_seq for t in turns if t.start_seq < cutoff and t.start_seq is not None]
        cutoff = max(previous) if previous else 0
    return cutoff


def redo_cursor(turns, cutoff, steps):
    for _ in range(steps):
        following = [t.start_seq for t in turns
                     if t.start_seq is not None and t.start_seq > cutoff]
        if not following:
            return None
        cutoff = min(following)
    return cutoff


# --------------------------------------------------------------------------
# writing A
# --------------------------------------------------------------------------

def snapshot_current_log(paths, keep):
    if not os.path.isfile(paths.log):
        return None
    os.makedirs(paths.snapshots, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    target = os.path.join(paths.snapshots, "A-%s-%d.zstd" % (stamp, int(time.time() * 1000) % 1000))
    shutil.copy2(paths.log, target)
    shots = sorted(glob.glob(os.path.join(paths.snapshots, "A-*.zstd")))
    for stale in shots[:-max(1, keep)]:
        try:
            os.remove(stale)
        except OSError:
            pass
    return target


def retire_projcache(paths):
    if not paths.projcache_record:
        return None
    os.makedirs(os.path.dirname(paths.projcache_record), exist_ok=True)
    target = "%s.bak-%s" % (paths.projcache_record,
                            time.strftime("%Y%m%d-%H%M%S"))
    os.replace(paths.projcache_record, target)
    return target


def prefix_bytes(events, cutoff):
    """Header + every event whose seq < cutoff, in log order."""
    out = []
    seen_seq = False
    for (raw, etype, seq, _) in events:
        if seq is None:
            out.append(raw)          # the session header has no seq
            continue
        if seq >= cutoff:
            break                    # the log is contiguous; nothing after this
        out.append(raw)
        seen_seq = True
    if not seen_seq and cutoff > 0:
        fail("cutoff %d keeps no events; the log's seq numbering looks broken" % cutoff)
    return b"".join(out)


def apply_cutoff(paths, events, cutoff, keep, note, dry_run):
    """Writes A = prefix(A-backup, cutoff). Returns a dict describing the write."""
    payload = prefix_bytes(events, cutoff)
    kept = len([1 for (raw, _, seq, _) in events if seq is None or seq < cutoff])
    result = {
        "cutoff": cutoff,
        "events_kept": kept,
        "bytes": len(payload),
        "written": False,
        "snapshot": None,
        "projcache_retired": None,
    }
    if dry_run:
        return result

    handle, tmp_path = tempfile.mkstemp(prefix=".rewind-", suffix=".jsonl", dir=paths.dir)
    os.close(handle)
    try:
        with open(tmp_path, "wb") as out:
            out.write(payload)
        compressed = tmp_path + ".zstd"
        compress_to(tmp_path, compressed)

        result["snapshot"] = snapshot_current_log(paths, keep)
        os.replace(compressed, paths.log)
        result["written"] = True
        result["projcache_retired"] = retire_projcache(paths)
        write_cursor(paths, cutoff, backup_total(events), note)
    finally:
        for leftover in (tmp_path, tmp_path + ".zstd"):
            if os.path.exists(leftover):
                try:
                    os.remove(leftover)
                except OSError:
                    pass
    return result


def log_is_live(paths, window):
    try:
        age = time.time() - os.path.getmtime(paths.log)
    except OSError:
        return False
    return age < window


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------

def cmd_status(paths, args):
    log_events = load_events(paths.log)
    log_seqs = [seq for (_, _, seq, _) in log_events if seq is not None]
    log_total = (max(log_seqs) + 1) if log_seqs else 0
    lines = []
    lines.append("session : %s" % paths.id)
    lines.append("log     : %s" % paths.log)
    lines.append("events  : %d (seq cutoff %d)" % (len(log_events), log_total))
    age = time.time() - os.path.getmtime(paths.log)
    lines.append("mtime   : %s (%.0fs ago)%s" % (
        time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(paths.log))),
        age, "  [live]" if age < args.live_window else ""))

    if not os.path.isfile(paths.backup):
        lines.append("backup  : (none - run `undo`/`init` to create A-backup)")
        state = None
    else:
        backup_events = load_events(paths.backup)
        total = backup_total(backup_events)
        state = read_cursor(paths)
        cursor = state["cursor"] if state else total
        turns, compactions = build_model(backup_events)
        lines.append("backup  : %s (%d events, cutoff %d)" % (
            os.path.basename(paths.backup), len(backup_events), total))
        lines.append("cursor  : %d  (%d turns kept of %d)" % (
            cursor, len([t for t in turns if t.start_seq < cursor]), len(turns)))
        expected = prefix_bytes(backup_events, cursor)
        log_bytes = decompress(paths.log)
        if log_bytes == expected:
            lines.append("state   : log == prefix(backup, cursor)  [in sync]")
        elif log_bytes.startswith(expected):
            lines.append("state   : log extends the cursor (live appends since the "
                         "last write); `adopt` keeps them, any undo/redo drops them")
        else:
            lines.append("state   : log DIVERGES from backup (edited elsewhere?)")

    markers = []
    _, compactions = build_model(log_events)
    for (etype, seq, obj) in compactions:
        if etype == "compaction/start":
            data = obj.get("data") or {}
            markers.append(seq)
            extra = []
            ident = data.get("compactionId")
            if ident:
                extra.append("id=%s" % str(ident)[:8])
            if data.get("turn") is not None:
                extra.append("turn=%s" % data.get("turn"))
            if data.get("shadowedSeqs") is not None:
                extra.append("shadowed=%s" % data.get("shadowedSeqs"))
            if data.get("shadowedTokenCount") is not None:
                extra.append("tokens=%s" % data.get("shadowedTokenCount"))
            lines.append("compact : seq %d%s" % (
                seq, ("  " + " ".join(extra)) if extra else ""))
    if not markers:
        lines.append("compact : none in the log")
    if paths.projcache_record:
        lines.append("projcache: present (retired on the next write)")
    else:
        lines.append("projcache: absent")
    print("\n".join(lines))
    if args.json:
        print(json.dumps({"log_events": len(log_events), "log_cutoff": log_total,
                          "compactions": markers}, ensure_ascii=False))


def cmd_list(paths, args):
    events = load_events(paths.log if not os.path.isfile(paths.backup) else paths.backup)
    turns, compactions = build_model(events)
    print("turns in %s" % ("A-backup" if os.path.isfile(paths.backup) else "log"))
    for turn in turns:
        stamp = ""
        if turn.start_time:
            stamp = time.strftime("%m-%d %H:%M", time.localtime(turn.start_time / 1000.0))
        print("  turn %2s  start seq %-6s end seq %-6s  %-11s  %s" % (
            turn.number, turn.start_seq, "-" if turn.end_seq is None else turn.end_seq,
            stamp, turn.first_user))
    if compactions:
        print("compaction markers")
        for (etype, seq, obj) in compactions:
            data = obj.get("data") or {}
            extra = ""
            if etype == "compaction/start":
                parts = []
                if data.get("compactionId"):
                    parts.append("id=%s" % str(data.get("compactionId"))[:8])
                if data.get("shadowedSeqs") is not None:
                    parts.append("shadowed=%s" % data.get("shadowedSeqs"))
                if data.get("shadowedTokenCount") is not None:
                    parts.append("tokens=%s" % data.get("shadowedTokenCount"))
                extra = ("  " + " ".join(parts)) if parts else ""
            print("  seq %-6s %-18s%s" % (seq, etype, extra))


def cmd_init(paths, args):
    events, created = ensure_backup(paths, args.dry_run)
    total = backup_total(events)
    state = read_cursor(paths)
    cursor = state["cursor"] if state else total
    if not args.dry_run:
        write_cursor(paths, cursor, total, "init")
    print("backup %s %s (%d events, cutoff %d)" % (
        "created" if created else "kept", paths.backup, len(events), total))
    print("cursor %d%s" % (cursor, "  [dry-run]" if args.dry_run else ""))


def maybe_auto_init(paths, args):
    if os.path.isfile(paths.backup):
        events = load_events(paths.backup)
        state = read_cursor(paths)
        if state:
            return events, state["cursor"]
        total = backup_total(events)
        if not args.dry_run:
            write_cursor(paths, total, total, "auto-init cursor")
        print("note: backup existed without a cursor; starting at the end (cutoff %d)" % total)
        return events, total
    events, _ = ensure_backup(paths, args.dry_run)
    total = backup_total(events)
    if not args.dry_run:
        write_cursor(paths, total, total, "auto-init from the current log")
    print("note: %s%s and started the cursor at the end (cutoff %d)" % (
        "would create " if args.dry_run else "created ",
        os.path.basename(paths.backup), total))
    return events, total


def run_move(paths, args, target_cutoff, note, events, previous_cursor):
    total = backup_total(events)
    cutoff = max(0, min(target_cutoff, total))
    if cutoff == previous_cursor:
        print("%s: cursor already at %d - nothing to do" % (note, cutoff))
        return
    result = apply_cutoff(paths, events, cutoff, args.keep, note, args.dry_run)
    turns, _ = build_model(events)
    print("%s: cursor %d -> %d  (%d events, %d turns kept of %d)%s" % (
        note, previous_cursor, result["cutoff"], result["events_kept"],
        len([t for t in turns if t.start_seq < result["cutoff"]]), len(turns),
        "  [dry-run]" if args.dry_run else ""))
    if result["written"]:
        print("log     : rewritten (%d bytes)%s" % (
            result["bytes"], "  snapshot %s" % os.path.basename(result["snapshot"])
            if result["snapshot"] else ""))
        if result["projcache_retired"]:
            print("projcache: retired -> %s" % os.path.basename(result["projcache_retired"]))
        print("resume  : reopen the session in DSH to load the new content "
              "(the running window keeps its copy)")


def cmd_undo(paths, args):
    events, cursor = maybe_auto_init(paths, args)
    turns, _ = build_model(events)
    target = undo_cursor(turns, cursor, args.steps)
    run_move(paths, args, target, "undo %d" % args.steps, events, cursor)


def cmd_redo(paths, args):
    events, cursor = maybe_auto_init(paths, args)
    turns, _ = build_model(events)
    total = backup_total(events)
    target = redo_cursor(turns, cursor, args.steps)
    if target is None:
        target = total
    if target == cursor and cursor < total:
        target = total
    run_move(paths, args, target, "redo %d" % args.steps, events, cursor)


def cmd_goto(paths, args):
    events, cursor = maybe_auto_init(paths, args)
    turns, compactions = build_model(events)
    total = backup_total(events)
    if args.turn is not None:
        match = [t for t in turns if t.number == args.turn]
        if not match:
            fail("no turn %d in the backup (1..%d)" % (args.turn, len(turns)))
        target = match[0].start_seq
        note = "goto turn %d" % args.turn
    elif args.event is not None:
        target = args.event
        note = "goto event %d" % args.event
    elif args.before_compaction:
        starts = [seq for (etype, seq, _) in compactions if etype == "compaction/start"]
        if not starts:
            fail("no compaction/start event in the backup")
        target = min(starts)
        note = "goto before compaction"
    elif args.start:
        target = 0
        note = "goto start"
    elif args.end:
        target = total
        note = "goto end"
    else:
        fail("goto needs one of --turn N / --event SEQ / --before-compaction / --start / --end")
    run_move(paths, args, target, note, events, cursor)


def cmd_restore(paths, args):
    events, cursor = maybe_auto_init(paths, args)
    run_move(paths, args, backup_total(events), "restore", events, cursor)


def cmd_verify(paths, args):
    if not os.path.isfile(paths.backup):
        fail("no A-backup yet")
    events = load_events(paths.backup)
    state = read_cursor(paths)
    cursor = state["cursor"] if state else backup_total(events)
    expected = prefix_bytes(events, cursor)
    actual = decompress(paths.log)
    if actual == expected:
        print("ok: log == prefix(backup, cursor=%d)  (%d bytes)" % (cursor, len(actual)))
        return
    if actual.startswith(expected):
        print("ok(extended): log starts with the expected prefix and has %d extra bytes "
              "(events appended after the last write)" % (len(actual) - len(expected)))
        return
    sys.exit(2)


def cmd_retire_cache(paths, args):
    """Retires the derived projection cache without touching the log."""
    if not paths.projcache_record:
        print("no projection cache record for %s" % paths.id)
        return
    if args.dry_run:
        print("would retire %s  [dry-run]" % paths.projcache_record)
        return
    retired = retire_projcache(paths)
    print("retired %s -> %s" % (paths.projcache_record, retired))
    print("note: DSH rebuilds its projection from the log on the next open. It "
          "rewrites the cache while the session is running, so run this with DSH "
          "closed, right before reopening the session.")


def cmd_adopt(paths, args):
    """Appends the events the log has beyond the backup (keeps live appends)."""
    if not os.path.isfile(paths.backup):
        fail("no A-backup yet; nothing to adopt into")
    backup_events = load_events(paths.backup)
    total = backup_total(backup_events)
    log_events = load_events(paths.log)
    log_seqs = [seq for (_, _, seq, _) in log_events if seq is not None]
    log_total = (max(log_seqs) + 1) if log_seqs else 0
    if log_total <= total:
        print("nothing to adopt: log cutoff %d <= backup cutoff %d" % (log_total, total))
        return
    appended = sorted(seq for seq in log_seqs if seq > total - 1)
    if appended and appended[0] > total:
        # A live session keeps appending with the seq counter it has in memory,
        # so after a truncation the tail can start past the backup's end.
        print("warning: the appended tail starts at seq %d, leaving a gap %d..%d. "
              "DSH reads the log in order; close the session before walking it "
              "back if you want a contiguous log." % (appended[0], total, appended[0] - 1))
    if args.dry_run:
        print("would adopt %d events (cutoff %d -> %d)  [dry-run]" % (
            log_total - total, total, log_total))
        return
    snapshot = snapshot_current_log(paths, args.keep)
    handle, tmp_path = tempfile.mkstemp(prefix=".adopt-", suffix=".jsonl", dir=paths.dir)
    os.close(handle)
    try:
        with open(tmp_path, "wb") as out:
            for (raw, _, seq, _) in log_events:
                out.write(raw)
        compress_to(tmp_path, paths.backup)
        write_cursor(paths, log_total, log_total, "adopt %d events" % (log_total - total))
    finally:
        if os.path.exists(tmp_path):
            try:
                os.remove(tmp_path)
            except OSError:
                pass
    print("adopted %d events: backup cutoff %d -> %d%s" % (
        log_total - total, total, log_total,
        "  (snapshot %s)" % os.path.basename(snapshot) if snapshot else ""))


# --------------------------------------------------------------------------
# argument parsing
# --------------------------------------------------------------------------

GLOBAL_OPTIONS = {
    "--session": 1, "--file": 1, "--sessions-root": 1, "--projcache-root": 1,
    "--live-window": 1, "--keep": 1,
    "--force": 0, "--dry-run": 0, "--json": 0,
}


def normalize_argv(argv):
    """Global options may be written after the command (`goto --start --force`)."""
    if not argv:
        return argv
    head, tail = [], []
    index = 0
    while index < len(argv):
        token = argv[index]
        name = token.split("=", 1)[0]
        if name in GLOBAL_OPTIONS:
            head.append(token)
            expects_value = GLOBAL_OPTIONS[name]
            if "=" not in token and expects_value and index + 1 < len(argv):
                head.append(argv[index + 1])
                index += 2
                continue
        else:
            tail.append(token)
        index += 1
    return head + tail


def build_parser():
    parser = argparse.ArgumentParser(
        description="undo / redo over a DSH session log (prefix regeneration)",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--session", help="session id (exact, unique prefix or substring)")
    parser.add_argument("--file", help="explicit path to session.v4.jsonl.zstd")
    parser.add_argument("--sessions-root", default=DEFAULT_SESSIONS_ROOT,
                        help="sessions root (default ~/.dsh/sessions)")
    parser.add_argument("--projcache-root", default=DEFAULT_PROJCACHE_ROOT,
                        help="projection cache root (default ~/.dsh/storages/session_projcache)")
    parser.add_argument("--force", action="store_true",
                        help="allow writing while the log looks live")
    parser.add_argument("--dry-run", action="store_true", help="print, write nothing")
    parser.add_argument("--keep", type=int, default=3,
                        help="A snapshots to keep in <session>/rewind (default 3)")
    parser.add_argument("--live-window", type=float, default=120.0,
                        help="seconds of log mtime that count as 'live' (default 120)")
    parser.add_argument("--json", action="store_true", help="machine-readable status")

    commands = parser.add_subparsers(dest="command")
    commands.add_parser("status", help="cursor, backup, log, compaction markers")
    commands.add_parser("list", help="numbered turns and compaction markers")

    commands.add_parser("init", help="create A-backup, cursor = end")
    undo = commands.add_parser("undo", help="move the cursor back N turns")
    undo.add_argument("steps", nargs="?", type=int, default=1)
    redo = commands.add_parser("redo", help="move the cursor forward N turns")
    redo.add_argument("steps", nargs="?", type=int, default=1)

    goto = commands.add_parser("goto", help="move the cursor to an explicit target")
    goto.add_argument("--turn", type=int)
    goto.add_argument("--event", type=int, help="seq cutoff: keep events with seq < N")
    goto.add_argument("--before-compaction", action="store_true")
    goto.add_argument("--start", action="store_true")
    goto.add_argument("--end", action="store_true")

    commands.add_parser("restore", help="cursor = end of A-backup")
    commands.add_parser("verify", help="check log == prefix(backup, cursor)")
    commands.add_parser("retire-cache",
                        help="drop the derived projection cache (run with DSH closed)")
    commands.add_parser("adopt", help="append events the log has beyond the backup")
    return parser


def main(argv):
    global ZSTD
    parser = build_parser()
    args = parser.parse_args(normalize_argv(argv))
    if not args.command:
        parser.print_help()
        return 0
    ZSTD = find_zstd()

    if args.file:
        log_path = os.path.abspath(args.file)
        if not os.path.isfile(log_path):
            fail("no such file: %s" % log_path)
        projcache_root = None
    elif args.session:
        log_path = resolve_session(args.session, args.sessions_root)
        projcache_root = args.projcache_root
    else:
        fail("give --session <id> or --file <path>")

    paths = SessionPaths(log_path, projcache_root)
    writing = args.command in ("undo", "redo", "goto", "restore", "init", "adopt",
                               "retire-cache")
    if writing and not args.dry_run and log_is_live(paths, args.live_window) and not args.force:
        fail("the log changed %.0fs ago - the session is probably live.\n"
             "       Close/switch away from it, or pass --force to write anyway\n"
             "       (the running window will keep its in-memory copy)."
             % (time.time() - os.path.getmtime(paths.log)))

    handlers = {
        "status": cmd_status,
        "list": cmd_list,
        "init": cmd_init,
        "undo": cmd_undo,
        "redo": cmd_redo,
        "goto": cmd_goto,
        "restore": cmd_restore,
        "verify": cmd_verify,
        "retire-cache": cmd_retire_cache,
        "adopt": cmd_adopt,
    }
    handlers[args.command](paths, args)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
