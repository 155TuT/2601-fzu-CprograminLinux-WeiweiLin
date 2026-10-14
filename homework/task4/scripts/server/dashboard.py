#!/usr/bin/env python3
"""Read-only session dashboard for the C stream_server (Python standard library).

Run from task4: python3 scripts/server/dashboard.py --state state/model-server
Use --once for a plain snapshot, --json for structured data, and --preview to
opt in to short question previews. No question/answer/thinking body is otherwise
read. The dashboard never creates state, acquires flock, or signals a worker.

Connection state comes from Linux /proc/locks, not old events or metrics.
"paused" means the lock-owning worker is stopped (T/t), not that a remote client
has been suspended. Metrics are periodic snapshots; ctx~ is the current attempt's
prompt_tokens + generated_tokens, not a live measurement of the model's KV cache.
done/cancelled markers take precedence over model metrics. An interrupted turn
remains pending; offline does not mean completed. Reads are bounded, nonblocking,
and reject symlinks/nonregular files. Races with normal atomic state updates are
shown as missing/unavailable information and retried on the next refresh.
"""

import argparse
import datetime
import json
import locale
import math
import os
from pathlib import Path
import re
import signal
import stat
import sys
import time
import unicodedata

SESSION = re.compile(r"[0-9a-f]{32}\Z")
TURN = re.compile(r"turn-([0-9]{8})\Z")
EVENT = re.compile(r"pid=(\d+) event=([a-z]+) request=(\d+) offset=(\d+)\Z")
MAX_SESSIONS, MAX_TURNS = 4096, 10000
MAX_METADATA, MAX_EVENTS, MAX_LOCKS = 65536, 65536, 4 * 1024 * 1024
DIRECTORY = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
READ_ONLY = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC


def clean(value, limit=160):
    """Make untrusted filenames/metadata harmless to terminal control sequences."""
    if not isinstance(value, str):
        return ""
    return "".join(" " if unicodedata.category(c).startswith("C") else c
                   for c in value[:limit]).strip()


def width(text):
    return sum(0 if unicodedata.combining(c) else
               2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in text)


def fit(text, cells, pad=False):
    """Crop by display cells, retaining whole UTF-8 characters on narrow screens."""
    text = clean(str(text), 4096)
    if cells <= 0:
        return ""
    if width(text) > cells:
        output, used = [], 0
        for character in text:
            count = width(character)
            if used + count > cells - 1:
                break
            output.append(character)
            used += count
        text = "".join(output) + "…"
    return text + " " * (cells - width(text)) if pad else text


def open_root(path):
    """Traverse every directory component without following a symlink."""
    parts = Path(os.path.abspath(os.path.expanduser(str(path)))).parts
    descriptor = os.open(parts[0], DIRECTORY)
    try:
        for part in parts[1:]:
            child = os.open(part, DIRECTORY, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def regular_info(directory, name, warnings):
    try:
        info = os.stat(name, dir_fd=directory, follow_symlinks=False)
    except FileNotFoundError:
        return None
    except OSError:
        warnings.append(name + ": unavailable")
        return None
    if not stat.S_ISREG(info.st_mode):
        warnings.append(name + ": nonregular file ignored")
        return None
    return info


def read_regular(directory, name, limit, warnings, *, tail=False, prefix=False):
    """O_NONBLOCK also prevents a raced-in FIFO from ever blocking this reader."""
    try:
        descriptor = os.open(name, READ_ONLY, dir_fd=directory)
    except FileNotFoundError:
        return None, None
    except OSError:
        warnings.append(name + ": unreadable or unsafe")
        return None, None
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode):
            warnings.append(name + ": nonregular file ignored")
            return None, None
        if info.st_size > limit and not (tail or prefix):
            warnings.append(name + ": size limit exceeded")
            return None, info
        offset = max(0, info.st_size - limit) if tail else 0
        data = os.pread(descriptor, limit + (not tail and not prefix), offset)
        if len(data) > limit:
            warnings.append(name + ": size limit exceeded")
            return None, info
        if tail and offset:
            data = data.partition(b"\n")[2]  # Discard a partial leading log record.
        return data, info
    except OSError:
        warnings.append(name + ": changed or unreadable")
        return None, None
    finally:
        os.close(descriptor)


def proc_read(path, limit):
    try:
        descriptor = os.open(path, READ_ONLY)
        try:
            if not stat.S_ISREG(os.fstat(descriptor).st_mode):
                return None
            data = bytearray()
            while len(data) <= limit:
                part = os.read(descriptor, min(65536, limit + 1 - len(data)))
                if not part:
                    return bytes(data)
                data.extend(part)
            return None
        finally:
            os.close(descriptor)
    except OSError:
        return None


def lock_snapshot():
    """Observe existing exclusive flock entries; never lock/unlock a state file."""
    content = proc_read("/proc/locks", MAX_LOCKS)
    if content is None:
        return None
    locks = {}
    for line in content.decode("ascii", "replace").splitlines():
        fields = line.split()
        if len(fields) < 8 or fields[1:4] != ["FLOCK", "ADVISORY", "WRITE"]:
            continue  # Ignore waiting (->), POSIX and unrelated locks.
        try:
            major, minor, inode = fields[5].split(":")
            key = (int(major, 16), int(minor, 16), int(inode))
            locks[key] = int(fields[4])
        except (ValueError, IndexError):
            continue
    return locks


def process_state(pid):
    if not isinstance(pid, int) or pid <= 0:
        return None
    content = proc_read(f"/proc/{pid}/stat", 8192)
    if content is None:
        return None
    fields = content.rsplit(b")", 1)[-1].split()
    return fields[0].decode("ascii", "replace") if fields else None


def number(document, key, *, integer=False):
    value = document.get(key)
    if type(value) not in (int, float) or value < 0:
        return None
    try:
        if not math.isfinite(value):
            return None
    except OverflowError:
        return None
    if integer:
        return int(value) if int(value) == value and value <= 2**63 - 1 else None
    return value


def read_session(parent, token, locks, now, preview):
    descriptor = os.open(token, DIRECTORY, dir_fd=parent)
    warnings = []
    try:
        lock = regular_info(descriptor, "session.lock", warnings)
        pid = None
        connection = "unknown"
        if lock is not None and locks is not None:
            pid = locks.get((os.major(lock.st_dev), os.minor(lock.st_dev), lock.st_ino))
            connection = "offline" if pid is None else "active"
            if process_state(pid) in ("T", "t"):
                connection = "paused"
        events, events_info = read_regular(descriptor, "events.log", MAX_EVENTS, warnings, tail=True)
        last_event = None
        if events:
            for line in reversed(events.split(b"\n")[:-1]):
                match = EVENT.fullmatch(line.decode("ascii", "replace"))
                if match:
                    last_event = {"pid": int(match[1]), "event": match[2],
                                  "request": int(match[3]), "offset": int(match[4])}
                    break
        turns = []
        with os.scandir(descriptor) as entries:
            for entry in entries:
                match = TURN.fullmatch(entry.name)
                if match and 1 <= int(match[1]) <= MAX_TURNS:
                    if entry.is_dir(follow_symlinks=False):
                        turns.append((int(match[1]), entry.name))
                    else:
                        warnings.append(entry.name + ": unsafe turn ignored")
                if len(turns) >= MAX_TURNS:
                    break
        latest = max(turns, default=(0, None))
        updated = events_info.st_mtime if events_info else os.fstat(descriptor).st_mtime
        item = {"session": token, "connection": connection, "worker_pid": pid,
                "worker_state": process_state(pid), "request": latest[0],
                "turn_count": len(turns), "phase": "idle", "terminal": False,
                "answer_bytes": 0, "thinking_bytes": 0, "generated_tokens": None,
                "prompt_tokens": None, "context_tokens": None, "context_used_estimate": None,
                "tokens_per_second": None, "metrics_age_seconds": None,
                "metrics_stale": False, "model": "", "backend": "", "stop_type": "",
                "error_summary": "", "last_event": last_event, "warnings": warnings}
        if latest[1]:
            turn = os.open(latest[1], DIRECTORY, dir_fd=descriptor)
            try:
                infos = {name: regular_info(turn, name, warnings) for name in
                         ("done", "cancelled", "response.txt", "thinking.txt", "metrics.json")}
                updated = max([updated] + [v.st_mtime for v in infos.values() if v])
                for field, name in (("answer_bytes", "response.txt"), ("thinking_bytes", "thinking.txt")):
                    if infos[name]:
                        item[field] = infos[name].st_size
                backend, _ = read_regular(turn, "backend.txt", 64, warnings)
                item["backend"] = clean(backend.decode("utf-8", "replace"), 64) if backend else "unknown"
                raw, info = read_regular(turn, "metrics.json", MAX_METADATA, warnings)
                metrics = {}
                if raw:
                    try:
                        value = json.loads(raw)
                        if not isinstance(value, dict):
                            raise ValueError("not an object")
                        metrics = value
                    except (ValueError, UnicodeError, RecursionError):
                        warnings.append("metrics.json: invalid snapshot")
                if info:
                    item["metrics_age_seconds"] = round(max(0, now - info.st_mtime), 1)
                timings = metrics.get("timings", {})
                if not isinstance(timings, dict):
                    timings = {}
                phase = clean(metrics.get("phase"), 32)
                generated = number(metrics, "generated_tokens", integer=True)
                # Native final totals override legacy counters that counted prompt-progress placeholders.
                if phase in ("completed", "error", "interrupted"):
                    native = number(timings, "predicted_n", integer=True)
                    if native is not None:
                        generated = native
                item.update(generated_tokens=generated,
                            prompt_tokens=number(metrics, "prompt_tokens", integer=True),
                            context_tokens=number(metrics, "context_tokens", integer=True),
                            tokens_per_second=number(metrics, "tokens_per_second"),
                            model=clean(metrics.get("model"), 64),
                            stop_type=clean(metrics.get("stop_type"), 32),
                            error_summary=clean(metrics.get("error")))
                if phase in ("completed", "error", "interrupted"):
                    rate = number(timings, "predicted_per_second")
                    if rate is not None:
                        item["tokens_per_second"] = rate
                if item["prompt_tokens"] is not None and generated is not None:
                    item["context_used_estimate"] = item["prompt_tokens"] + generated
                if infos["cancelled"]:
                    item.update(phase="cancelled", terminal=True)
                elif infos["done"]:
                    item.update(phase="completed", terminal=True)
                elif phase in ("error", "interrupted"):
                    item["phase"] = "error"
                elif item["answer_bytes"]:
                    item["phase"] = "generating"
                elif item["thinking_bytes"]:
                    item["phase"] = "thinking"
                elif phase in ("loading", "processing", "replaying") or item["backend"] == "llama":
                    item["phase"] = "loading"
                else:
                    item["phase"] = "generating"
                item["metrics_stale"] = bool(metrics and not item["terminal"] and
                    (connection in ("offline", "unknown", "paused") or
                     (item["metrics_age_seconds"] is not None and item["metrics_age_seconds"] > 5)))
                if preview:
                    prompt, _ = read_regular(turn, "prompt.txt", 512, warnings, prefix=True)
                    item["prompt_preview"] = clean(prompt.decode("utf-8", "replace"), 64) if prompt else ""
            finally:
                os.close(turn)
        item["updated_at"] = updated
        return item
    finally:
        os.close(descriptor)


def snapshot(path, preview=False):
    now = time.time()
    document = {"schema": 1, "state": os.path.abspath(os.path.expanduser(str(path))),
                "observed_at": datetime.datetime.fromtimestamp(now, datetime.timezone.utc).isoformat(),
                "sessions": [], "warnings": []}
    descriptor = open_root(path)
    try:
        locks = lock_snapshot()
        document["lock_observation_available"] = locks is not None
        if locks is None:
            document["warnings"].append("/proc/locks unavailable; connection state unknown")
        with os.scandir(descriptor) as entries:
            for entry in entries:
                if not SESSION.fullmatch(entry.name):
                    continue
                if not entry.is_dir(follow_symlinks=False):
                    document["warnings"].append(entry.name[:12] + ": unsafe session ignored")
                    continue
                try:
                    document["sessions"].append(read_session(descriptor, entry.name, locks, now, preview))
                except OSError:
                    document["warnings"].append(entry.name[:12] + ": changed or unreadable; retry next refresh")
                if len(document["sessions"]) >= MAX_SESSIONS:
                    document["warnings"].append("session scan limit reached")
                    break
        document["sessions"].sort(key=lambda s: (-s["updated_at"], s["session"]))
        return document
    finally:
        os.close(descriptor)


def formatted(value, suffix=""):
    if value is None:
        return "-"
    if type(value) is float:
        return f"{value:.1f}{suffix}"
    return str(value) + suffix


def layout(document, columns, rows, selected=0):
    """Return cell-bounded lines independently of curses, also used by --once."""
    columns, rows = max(1, columns), max(1, rows)
    sessions = document["sessions"]
    counts = {kind: sum(s["connection"] == kind for s in sessions)
              for kind in ("active", "paused", "offline", "unknown")}
    lines = [("title", "Task4 server | read-only session dashboard"),
             ("secondary", f"{len(sessions)} sessions  active {counts['active']}  paused {counts['paused']}  offline {counts['offline']}  unknown {counts['unknown']}"),
             ("secondary", "State: " + document["state"])]
    fields = [("session", "SESSION", 12), ("connection", "LINK", 7), ("request", "TURN", 4), ("phase", "PHASE", 10)]
    if columns >= 60:
        fields.extend([("generated_tokens", "TOKENS", 7), ("answer_bytes", "ANSWER B", 8)])
    if columns >= 82:
        fields.extend([("context", "CTX~", 12), ("tokens_per_second", "TOK/S", 6)])
    if columns >= 110:
        fields.extend([("worker_pid", "WORKER", 7), ("age", "METRIC AGE", 10)])
    if columns >= 137:
        fields.append(("thinking_bytes", "THINK B", 8))
    lines.append(("title", " ".join(fit(label, size, True) for _, label, size in fields)))
    capacity = max(1, rows - (12 if rows >= 15 else 5))
    selected = max(0, min(selected, len(sessions) - 1))
    start = max(0, selected - capacity + 1)
    for index, session in enumerate(sessions[start:start + capacity], start):
        values = dict(session)
        values["session"] = session["session"][:12]
        values["phase"] += "*" if session["metrics_stale"] else ""
        values["context"] = formatted(session["context_used_estimate"]) + "/" + formatted(session["context_tokens"])
        values["age"] = formatted(session["metrics_age_seconds"], "s")
        row = " ".join(fit(formatted(values.get(key)), size, True) for key, _, size in fields)
        lines.append(("selected" if index == selected else "body", row))
    if not sessions:
        lines.append(("secondary", "No session directories yet."))
    if rows >= 15 and sessions:
        item = sessions[selected]
        lines.extend([("secondary", "-" * min(columns, 140)),
                      ("title", f"Selected {item['session']} | {item['connection']} / {item['phase']} | turn {item['request']}"),
                      ("body", f"Backend {item['backend'] or '-'}  model {item['model'] or '-'}  worker {formatted(item['worker_pid'])}  state {item['worker_state'] or '-'}"),
                      ("body", f"Answer {item['answer_bytes']} B  thinking {item['thinking_bytes']} B  tokens {formatted(item['generated_tokens'])}  ctx~ {formatted(item['context_used_estimate'])}/{formatted(item['context_tokens'])}"),
                      ("secondary", f"Metric age {formatted(item['metrics_age_seconds'], 's')}  rate {formatted(item['tokens_per_second'])} tok/s  stop {item['stop_type'] or '-'}  terminal {'yes' if item['terminal'] else 'no'}")])
        if item["error_summary"]:
            lines.append(("body", "Error: " + item["error_summary"]))
        elif item.get("prompt_preview"):
            lines.append(("body", "Question: " + item["prompt_preview"]))
        elif item["warnings"]:
            lines.append(("secondary", "Read: " + "; ".join(item["warnings"][:2])))
    if document["warnings"]:
        lines.append(("secondary", "Read: " + "; ".join(document["warnings"][:2])))
    lines = lines[:max(0, rows - 2)]
    lines.extend([("secondary", "* snapshot; link from flock; paused = worker stopped; ctx~ = attempt estimate"),
                  ("secondary", "Up/Down or j/k select | q quit | refresh 1s | previews opt-in")])
    return [(role, fit(text, columns)) for role, text in lines[:rows]]


def watch(screen, args):
    import curses
    screen.keypad(True)
    screen.timeout(100)
    try:
        curses.curs_set(0)
    except curses.error:
        pass
    attributes = {name: 0 for name in ("body", "secondary", "title", "selected")}
    if curses.has_colors():
        curses.start_color()
        curses.init_pair(1, curses.COLOR_BLACK, curses.COLOR_WHITE)
        curses.init_pair(2, 245 if curses.COLORS >= 256 else curses.COLOR_BLACK, curses.COLOR_WHITE)
        curses.init_pair(3, curses.COLOR_BLACK, 252 if curses.COLORS >= 256 else curses.COLOR_WHITE)
        attributes.update(body=curses.color_pair(1), secondary=curses.color_pair(2),
                          title=curses.color_pair(1) | curses.A_BOLD,
                          selected=curses.color_pair(3) | curses.A_BOLD)
        screen.bkgd(" ", attributes["body"])
    else:
        attributes.update(title=curses.A_BOLD, selected=curses.A_REVERSE)
    selected_token, selected, document, next_refresh = None, 0, None, 0
    redraw = True
    while True:
        now = time.monotonic()
        if document is None or now >= next_refresh:
            try:
                document = snapshot(args.state, args.preview)
            except OSError as error:
                document = {"state": str(args.state), "sessions": [], "warnings": [clean(str(error))]}
            if selected_token:
                selected = next((i for i, s in enumerate(document["sessions"]) if s["session"] == selected_token), selected)
            selected = max(0, min(selected, len(document["sessions"]) - 1))
            next_refresh = now + 1
            redraw = True
        if redraw:
            screen.erase()
            rows, columns = screen.getmaxyx()
            for row, (role, text) in enumerate(layout(document, max(1, columns - 1), rows, selected)):
                try:
                    screen.addstr(row, 0, text, attributes[role])
                except curses.error:
                    pass  # Resizes can invalidate coordinates between getmaxyx/addstr.
            screen.refresh()
            redraw = False
        key = screen.getch()
        if key in (ord("q"), ord("Q"), 27):
            return
        if key in (curses.KEY_DOWN, ord("j")):
            selected = min(selected + 1, len(document["sessions"]) - 1)
            redraw = True
        elif key in (curses.KEY_UP, ord("k")):
            selected = max(0, selected - 1)
            redraw = True
        elif key == curses.KEY_RESIZE:
            redraw = True
        if document["sessions"]:
            selected_token = document["sessions"][max(0, selected)]["session"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--state", type=Path, default=Path("state/server"), help="existing C server state directory")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--once", action="store_true", help="print one plain snapshot and exit")
    mode.add_argument("--json", action="store_true", help="print one JSON snapshot and exit")
    parser.add_argument("--preview", action="store_true", help="opt in to a question preview, at most 64 characters")
    args = parser.parse_args()
    try:
        if args.json or args.once:
            document = snapshot(args.state, args.preview)
            if args.json:
                print(json.dumps(document, ensure_ascii=False, indent=2, allow_nan=False))
            else:
                for _, line in layout(document, 140, max(24, len(document["sessions"]) + 14)):
                    print(line)
            return 0
        if not sys.stdin.isatty() or not sys.stdout.isatty():
            parser.error("interactive dashboard requires a terminal; use --once or --json")
        # Validate before entering curses; an invalid path creates no directory.
        descriptor = open_root(args.state)
        os.close(descriptor)
        locale.setlocale(locale.LC_ALL, "")
        import curses
        def interrupt_handler(_signum, _frame):
            raise KeyboardInterrupt
        old = {signum: signal.getsignal(signum) for signum in (signal.SIGTERM, signal.SIGHUP)}
        for signum in old:
            signal.signal(signum, interrupt_handler)
        try:
            try:
                curses.wrapper(watch, args)
            except curses.error as error:
                print("dashboard: " + clean(str(error), 240), file=sys.stderr)
                return 1
        finally:
            for signum, handler in old.items():
                signal.signal(signum, handler)
        return 0
    except KeyboardInterrupt:
        return 0
    except (OSError, ValueError) as error:
        print("dashboard: " + clean(str(error), 240), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
