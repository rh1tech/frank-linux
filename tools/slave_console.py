"""
slave_console.py - attach to the slave's USB console across a reset.

Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
SPDX-License-Identifier: GPL-3.0-or-later

Resetting the slave takes its USB device away and brings a new one back, so the
node that exists at reset time is not the node to read from. Opening the stale
one succeeds and then fails on the first read with ENXIO. Wait for the old node
to go, then for a new one to appear, and reopen if it vanishes mid-session.
"""

import glob, subprocess, time

import serial

PATTERN = "/dev/cu.usbmodemFRANK*"


def reset_slave():
    subprocess.run(["bash", "-c",
                    'source tools/probe.sh; oocd slave "init" "reset run" "exit" '
                    '>/dev/null 2>&1'], check=False)


class Console:
    def __init__(self, settle=3.0, timeout=45.0):
        # Let the pre-reset node disappear before believing anything we see.
        time.sleep(settle)
        self.port = None
        self.buf = b""
        self._open(timeout)

    def _open(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            for node in glob.glob(PATTERN):
                try:
                    self.port = serial.Serial(node, 115200, timeout=0.3)
                    return
                except Exception:
                    pass
            time.sleep(0.4)
        raise SystemExit("no slave console appeared")

    def drain(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                self.buf += self.port.read(4096)
            except Exception:
                # The device went away; give it a chance to come back rather
                # than reporting a boot failure that did not happen.
                try:
                    self.port.close()
                except Exception:
                    pass
                self._open(15)

    def send(self, cmd, wait=3.0):
        self.port.write((cmd + "\n").encode())
        self.port.flush()
        self.drain(wait)

    def text(self):
        return self.buf.decode("utf-8", "replace")
