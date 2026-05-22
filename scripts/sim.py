#!/usr/bin/env python3
"""sim.py - wrapper around the PDN native_cli simulator's --headless mode.

Manages a long-lived sim process so scenarios can issue commands and observe
events from independent shell invocations.
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BINARY = REPO_ROOT / ".pio" / "build" / "native_cli" / "program"
RUNTIME_DIR = REPO_ROOT / "scripts" / "sim"
PID_FILE = RUNTIME_DIR / "sim.pid"
FIFO = RUNTIME_DIR / "sim.in"
LOG = RUNTIME_DIR / "sim.log"
CURSOR_FILE = RUNTIME_DIR / "cursor"

READY_PATTERN = re.compile(r"^ready devices=\d+ pid=\d+\s*$")


def _is_running():
    if not PID_FILE.exists():
        return False
    try:
        pid = int(PID_FILE.read_text().strip())
    except (ValueError, OSError):
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def _read_pid():
    return int(PID_FILE.read_text().strip())


def _cleanup_runtime():
    for p in (PID_FILE, FIFO, LOG, CURSOR_FILE):
        try:
            p.unlink()
        except FileNotFoundError:
            pass


def _cursor():
    if CURSOR_FILE.exists():
        try:
            return int(CURSOR_FILE.read_text().strip())
        except ValueError:
            return 0
    return 0


def _set_cursor(offset):
    CURSOR_FILE.write_text(str(offset))


def _read_log_from(start_pos, timeout=None, line_filter=None, advance_cursor=False):
    """Yield log lines starting at start_pos, blocking until timeout.

    When advance_cursor is True, the persistent cursor is updated after each
    yielded line so subsequent callers start after the last consumed event.
    When False, the cursor is never touched (used for command responses).
    """
    deadline = None if timeout is None else time.monotonic() + timeout
    pos = start_pos
    pending = b""
    while True:
        size = LOG.stat().st_size if LOG.exists() else 0
        if size > pos:
            with LOG.open("rb") as f:
                f.seek(pos)
                chunk = f.read(size - pos)
            pending += chunk
            pos = size
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace")
                if line_filter is None or line_filter(line):
                    if advance_cursor:
                        consumed_end = pos - len(pending)
                        _set_cursor(consumed_end)
                    yield line
        if deadline is not None and time.monotonic() >= deadline:
            return
        time.sleep(0.05)


def _read_log_from_cursor(timeout=None, line_filter=None):
    """Yield log lines starting at cursor, advancing cursor on each yielded line."""
    yield from _read_log_from(
        _cursor(), timeout=timeout, line_filter=line_filter, advance_cursor=True
    )


def cmd_start(args):
    if _is_running():
        print(f"sim already running (pid {_read_pid()})", file=sys.stderr)
        return 1
    if not BINARY.exists():
        print(f"binary not found at {BINARY} -- build with: pio run -e native_cli", file=sys.stderr)
        return 1

    _cleanup_runtime()
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    os.mkfifo(FIFO)
    _set_cursor(0)

    log_fh = LOG.open("w")
    # Open the fifo for reading first so the sim's stdin redirect doesn't block.
    # We keep a write-end open in the parent so subsequent verbs can append.
    fifo_w = os.open(FIFO, os.O_RDWR | os.O_NONBLOCK)
    proc = subprocess.Popen(
        [str(BINARY), "--headless", str(args.devices)],
        stdin=open(FIFO, "rb"),
        stdout=log_fh,
        stderr=subprocess.DEVNULL,
        cwd=str(REPO_ROOT),
    )
    os.close(fifo_w)
    PID_FILE.write_text(str(proc.pid))

    deadline = time.monotonic() + args.ready_timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            tail = LOG.read_text().splitlines()[-20:]
            print("sim exited before ready", file=sys.stderr)
            for line in tail:
                print("  " + line, file=sys.stderr)
            _cleanup_runtime()
            return 1
        for line in _read_log_from_cursor(timeout=0.1):
            if READY_PATTERN.match(line):
                print(line)
                return 0
        time.sleep(0.05)

    print("timeout waiting for ready line", file=sys.stderr)
    tail = LOG.read_text().splitlines()[-20:]
    for line in tail:
        print("  " + line, file=sys.stderr)
    try:
        proc.terminate()
    except ProcessLookupError:
        pass
    _cleanup_runtime()
    return 1


def cmd_stop(_args):
    if not _is_running():
        _cleanup_runtime()
        return 0
    pid = _read_pid()
    try:
        with FIFO.open("w") as f:
            f.write("quit\n")
    except OSError:
        pass
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            break
        time.sleep(0.05)
    else:
        try:
            os.kill(pid, signal.SIGTERM)
            time.sleep(0.5)
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    _cleanup_runtime()
    return 0


def _send_and_capture_response(line, timeout=5.0):
    if not _is_running():
        print("sim not running", file=sys.stderr)
        return 1, None
    # Snapshot log size before sending so we only look at new output for this
    # command's response. The persistent cursor is not touched: event lines that
    # appear between here and the ok/err response remain available for
    # wait-event calls.
    tail_start = LOG.stat().st_size if LOG.exists() else 0
    with FIFO.open("w") as f:
        f.write(line + "\n")
    for log_line in _read_log_from(
        tail_start,
        timeout=timeout,
        line_filter=lambda l: l.startswith("ok:") or l.startswith("err:"),
        advance_cursor=False,
    ):
        print(log_line)
        return (0 if log_line.startswith("ok:") else 1), log_line
    print(f"timeout waiting for response to: {line}", file=sys.stderr)
    return 124, None


def cmd_cmd(args):
    line = " ".join(args.words)
    rc, _ = _send_and_capture_response(line, timeout=args.timeout)
    return rc


def cmd_wait_event(args):
    if not _is_running():
        print("sim not running", file=sys.stderr)
        return 1
    if args.regex:
        rx = re.compile(args.pattern)
        match = lambda l: bool(rx.search(l))
    else:
        match = lambda l: args.pattern in l
    for log_line in _read_log_from_cursor(
        timeout=args.timeout,
        line_filter=lambda l: l.startswith("event ") and match(l),
    ):
        print(log_line)
        return 0
    print(f"timeout waiting for event matching: {args.pattern}", file=sys.stderr)
    return 124


def cmd_events(args):
    if not LOG.exists():
        return 0
    pattern = args.match
    rx = re.compile(pattern) if pattern else None
    since = args.since_ms
    with LOG.open("r") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.startswith("event "):
                continue
            if since is not None:
                m = re.search(r"\bts=(\d+)\b", line)
                if not m or int(m.group(1)) < since:
                    continue
            if rx and not rx.search(line):
                continue
            print(line)
    return 0


def cmd_status(_args):
    if _is_running():
        print(f"running pid={_read_pid()}")
        print(f"runtime_dir={RUNTIME_DIR}")
        return 0
    print("not running")
    return 0


def main():
    p = argparse.ArgumentParser(prog="sim.py", description=__doc__)
    sub = p.add_subparsers(dest="verb", required=True)

    s = sub.add_parser("start")
    s.add_argument("devices", nargs="?", type=int, default=2)
    s.add_argument("--ready-timeout", type=float, default=10.0)
    s.set_defaults(func=cmd_start)

    s = sub.add_parser("stop")
    s.set_defaults(func=cmd_stop)

    s = sub.add_parser("cmd")
    s.add_argument("words", nargs="+")
    s.add_argument("--timeout", type=float, default=5.0)
    s.set_defaults(func=cmd_cmd)

    s = sub.add_parser("wait-event")
    s.add_argument("pattern")
    s.add_argument("--timeout", type=float, default=5.0)
    s.add_argument("--regex", action="store_true")
    s.set_defaults(func=cmd_wait_event)

    s = sub.add_parser("events")
    s.add_argument("--match")
    s.add_argument("--since-ms", type=int)
    s.set_defaults(func=cmd_events)

    s = sub.add_parser("status")
    s.set_defaults(func=cmd_status)

    # Shortcuts (sugar over cmd)
    for name, builder in [
        ("state",       lambda a: ["state"]),
        ("cable",       lambda a: ["cable", str(a.a), str(a.b)]),
        ("disconnect",  lambda a: ["disconnect", str(a.n)]),
        ("press",       lambda a: ["press", str(a.n), a.button]),
        ("longpress",   lambda a: ["longpress", str(a.n), str(a.ms)]),
        ("http",        lambda a: ["http", a.mode]),
        ("tick",        lambda a: ["tick", str(a.ms)]),
        ("add",         lambda a: ["add", a.role] if a.role else ["add"]),
    ]:
        sp = sub.add_parser(name)
        if name == "cable":
            sp.add_argument("a", type=int); sp.add_argument("b", type=int)
        elif name == "disconnect":
            sp.add_argument("n", type=int)
        elif name == "press":
            sp.add_argument("n", type=int); sp.add_argument("button", choices=["primary", "secondary"], default="primary", nargs="?")
        elif name == "longpress":
            sp.add_argument("n", type=int); sp.add_argument("ms", type=int)
        elif name == "http":
            sp.add_argument("mode", choices=["online", "offline"])
        elif name == "tick":
            sp.add_argument("ms", type=int)
        elif name == "add":
            sp.add_argument("role", choices=["hunter", "bounty"], nargs="?")
        sp.set_defaults(func=lambda args, b=builder: _send_and_capture_response(" ".join(b(args)))[0])

    args = p.parse_args()
    rc = args.func(args)
    sys.exit(rc if rc is not None else 0)


if __name__ == "__main__":
    main()
