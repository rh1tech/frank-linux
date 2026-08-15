#!/usr/bin/env python3
"""
console.py - serial capture and injection for one board half.

The probe's CDC bridge is the console channel that does not depend on anything
we are building: it keeps working when the link is down, when Linux has not
booted, and when the target is wedged. Every gate in this project reads from
here or from the HDMI capture, and nothing else.

Capture appends rather than truncates, and stamps each line with seconds since
capture start. Boot-time questions are almost always "how long after reset did
this stop", and a log without timestamps cannot answer that.

Usage:
    console.py capture <role> [--out FILE] [--seconds N] [--quiet]
    console.py send    <role> <text>        (\\n, \\r, \\t, \\xNN escapes honoured)
    console.py wait    <role> <pattern> [--timeout N] [--out FILE]
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("console.py needs pyserial: pip3 install pyserial")

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_LOG_DIR = os.path.join(os.path.dirname(HERE), "logs")


def resolve(role: str) -> tuple[str, int]:
    """Role -> (tty, baud). Both come from the bench config + live USB tree."""
    out = subprocess.run(
        [sys.executable, os.path.join(HERE, "devices.py"), "resolve", role],
        capture_output=True, text=True,
    )
    if out.returncode != 0:
        sys.exit(out.stderr.strip() or f"cannot resolve role '{role}'")

    tty = ""
    for line in out.stdout.splitlines():
        if line.startswith(f"{role.upper()}_TTY="):
            tty = line.split("=", 1)[1]

    baud = 115200
    conf = os.environ.get("FRANK_BENCH_CONF") or os.path.join(HERE, "bench.conf")
    if os.path.exists(conf):
        key = f"{role.upper()}_CONSOLE_BAUD"
        with open(conf) as fh:
            for raw in fh:
                line = raw.split("#", 1)[0].strip()
                if line.startswith(key + "="):
                    baud = int(line.split("=", 1)[1].strip())
    return tty, baud


def open_port(role: str) -> serial.Serial:
    tty, baud = resolve(role)
    try:
        # Short read timeout so a quiet line still lets the loop check its
        # deadline; without it a target that says nothing hangs until killed.
        return serial.Serial(tty, baud, timeout=0.2)
    except serial.SerialException as exc:
        sys.exit(f"cannot open {tty} for {role}: {exc}")


def log_path(role: str, explicit: str | None) -> str:
    if explicit:
        return explicit
    os.makedirs(DEFAULT_LOG_DIR, exist_ok=True)
    return os.path.join(DEFAULT_LOG_DIR, f"{role}.log")


def stream(port: serial.Serial, sink, deadline: float | None,
           pattern: re.Pattern | None, echo: bool) -> bool:
    """
    Pump bytes to `sink` until the deadline, or until `pattern` matches.

    Matching is done per accumulated line, not per read: a serial read returns
    whatever bytes happened to arrive, so a pattern straddling two reads would
    never match if we tested each read in isolation.

    Returns True if the pattern matched.
    """
    start = time.monotonic()
    line = ""
    last_rx = time.monotonic()
    while deadline is None or time.monotonic() < deadline:
        chunk = port.read(4096)
        if not chunk:
            # Flush a partial line once the target goes quiet.
            #
            # A shell prompt has no trailing newline, so a writer that only
            # emits on '\n' never records it: the log stops at the last kernel
            # message and a perfectly healthy system looks like it died just
            # before reaching userspace. Anything held for longer than a line
            # could plausibly take to arrive is a prompt, and belongs in the log.
            if line and time.monotonic() - last_rx > 0.5:
                stamp = time.monotonic() - start
                sink.write(f"[{stamp:9.3f}] {line}\n")
                sink.flush()
                if echo:
                    print(f"[{stamp:9.3f}] {line}", flush=True)
                line = ""
            continue
        last_rx = time.monotonic()
        text = chunk.decode("utf-8", errors="replace")
        for ch in text:
            if ch == "\n":
                stamp = time.monotonic() - start
                sink.write(f"[{stamp:9.3f}] {line}\n")
                sink.flush()
                if echo:
                    print(f"[{stamp:9.3f}] {line}", flush=True)
                if pattern and pattern.search(line):
                    return True
                line = ""
            elif ch != "\r":
                line += ch
        # A shell prompt has no trailing newline, so a pattern that only ever
        # appears as a prompt would never be tested if we matched whole lines
        # only. Test the partial line too.
        if pattern and pattern.search(line):
            stamp = time.monotonic() - start
            sink.write(f"[{stamp:9.3f}] {line}\n")
            sink.flush()
            if echo:
                print(f"[{stamp:9.3f}] {line}", flush=True)
            return True
    return False


def cmd_capture(args) -> int:
    port = open_port(args.role)
    path = log_path(args.role, args.out)
    deadline = time.monotonic() + args.seconds if args.seconds else None
    with open(path, "a") as sink:
        sink.write(f"\n===== capture {args.role} @ {time.strftime('%F %T')} =====\n")
        try:
            stream(port, sink, deadline, None, echo=not args.quiet)
        except KeyboardInterrupt:
            pass
    return 0


def cmd_send(args) -> int:
    port = open_port(args.role)
    payload = args.text.encode().decode("unicode_escape").encode("latin-1")
    port.write(payload)
    port.flush()
    return 0


def cmd_wait(args) -> int:
    port = open_port(args.role)
    path = log_path(args.role, args.out)
    pattern = re.compile(args.pattern)
    with open(path, "a") as sink:
        sink.write(f"\n===== wait {args.role} /{args.pattern}/ "
                   f"@ {time.strftime('%F %T')} =====\n")
        hit = stream(port, sink, time.monotonic() + args.timeout,
                     pattern, echo=not args.quiet)
    if hit:
        print(f"PASS  {args.role}: matched /{args.pattern}/")
        return 0
    print(f"FAIL  {args.role}: /{args.pattern}/ not seen in {args.timeout}s "
          f"(log: {path})", file=sys.stderr)
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("capture", help="stream the console to a log")
    c.add_argument("role")
    c.add_argument("--out")
    c.add_argument("--seconds", type=float, default=0)
    c.add_argument("--quiet", action="store_true")
    c.set_defaults(fn=cmd_capture)

    s = sub.add_parser("send", help="write text to the console")
    s.add_argument("role")
    s.add_argument("text")
    s.set_defaults(fn=cmd_send)

    w = sub.add_parser("wait", help="assert a pattern appears, with a timeout")
    w.add_argument("role")
    w.add_argument("pattern")
    w.add_argument("--timeout", type=float, default=30)
    w.add_argument("--out")
    w.add_argument("--quiet", action="store_true")
    w.set_defaults(fn=cmd_wait)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
