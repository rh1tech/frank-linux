#!/usr/bin/env python3
"""
devices.py - resolve bench instruments to stable addresses.

Two CMSIS-DAP probes and a capture card share this machine, and neither
/dev/cu.usbmodem* numbering nor probe enumeration order is stable across
replugs or reboots. Everything downstream therefore addresses hardware by USB
serial number, which is burned into the probe and never changes.

The registry walk is the only reliable way to pair a probe with its CDC UART
bridge: the tty node name is derived from the USB location ID, so it moves when
the device moves on the hub tree, but the tty node lives in the registry subtree
of the USB device that owns it. We parse ioreg's XML output rather than its
tree-drawing text output because the ASCII art is not a stable interface.

Usage:
    devices.py list                 every USB device with a serial, and its ttys
    devices.py resolve <role>       shell-evalable env for one role
    devices.py check                verify every role in bench.conf is present
"""

from __future__ import annotations

import glob
import json
import os
import plistlib
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Overridable so the swapped-probe guard can be tested against a deliberately
# wrong config without editing the real one.
CONF = os.environ.get("FRANK_BENCH_CONF") or os.path.join(HERE, "bench.conf")

# ioreg depth. The CDC tty node sits several layers below the USB device:
# device -> interface -> IOUSBHostInterface -> ... -> IOSerialBSDClient, and
# every hub between the Mac and the probe adds a level. Measured on this bench:
# depth 12 finds the probe on a short hub chain but silently misses the one two
# hubs deeper, reporting it as having no console at all. 16 is enough today and
# 24 leaves room for another hub; ioreg costs nothing, so buy the headroom.
# cmd_check() cross-checks against /dev to catch this going wrong again.
IOREG_DEPTH = "24"


def ioreg_usb_tree() -> list:
    """USB device subtrees as parsed plists."""
    out = subprocess.run(
        ["ioreg", "-a", "-l", "-w0", "-r", "-c", "IOUSBHostDevice", "-d", IOREG_DEPTH],
        capture_output=True,
        check=True,
    ).stdout
    if not out.strip():
        return []
    return plistlib.loads(out)


def collect_ttys(node: dict, out: list) -> None:
    """Every IOCalloutDevice anywhere in this node's subtree."""
    tty = node.get("IOCalloutDevice")
    if tty:
        out.append(tty)
    for child in node.get("IORegistryEntryChildren", []) or []:
        collect_ttys(child, out)


def walk(node: dict, devices: dict) -> None:
    """
    Record every node that carries a USB serial number, with the tty nodes in
    its subtree.

    A composite device repeats its serial number on each interface, so the same
    serial is seen many times. The outermost sighting owns the whole subtree and
    therefore sees every tty; keeping the entry with the most ttys collapses the
    duplicates without having to reason about registry depth.
    """
    serial = node.get("USB Serial Number")
    if serial:
        ttys: list[str] = []
        collect_ttys(node, ttys)
        prev = devices.get(serial)
        if prev is None or len(ttys) > len(prev["ttys"]):
            devices[serial] = {
                "serial": serial,
                "product": node.get("USB Product Name", "?"),
                "vendor": node.get("USB Vendor Name", "?"),
                "ttys": sorted(set(ttys)),
            }
    for child in node.get("IORegistryEntryChildren", []) or []:
        walk(child, devices)


def discover() -> dict:
    devices: dict = {}
    for root in ioreg_usb_tree():
        walk(root, devices)
    return devices


def load_conf() -> dict:
    """bench.conf: KEY=value, '#' comments. Absent file is not an error."""
    conf: dict = {}
    if not os.path.exists(CONF):
        return conf
    with open(CONF) as fh:
        for raw in fh:
            line = raw.split("#", 1)[0].strip()
            if not line or "=" not in line:
                continue
            key, val = line.split("=", 1)
            conf[key.strip()] = val.strip().strip('"').strip("'")
    return conf


def cmd_list() -> int:
    devices = discover()
    if not devices:
        print("no USB devices with serial numbers found", file=sys.stderr)
        return 1
    width = max(len(d["product"]) for d in devices.values())
    for dev in sorted(devices.values(), key=lambda d: d["product"]):
        ttys = ", ".join(dev["ttys"]) if dev["ttys"] else "-"
        print(f"{dev['product']:<{width}}  {dev['serial']:<18}  {ttys}")
    return 0


def cmd_resolve(role: str) -> int:
    """
    Emit shell env for one role. Roles are named in bench.conf as
    <ROLE>_PROBE_SERIAL; the tty is discovered, never configured, because it
    changes and the serial does not.
    """
    conf = load_conf()
    key = f"{role.upper()}_PROBE_SERIAL"
    serial = conf.get(key)
    if not serial:
        print(f"{key} is not set in {CONF}", file=sys.stderr)
        return 2

    dev = discover().get(serial)
    if dev is None:
        print(f"probe {serial} ({role}) is not attached", file=sys.stderr)
        return 3

    # A CMSIS-DAP probe exposes exactly one CDC UART bridge. More than one means
    # we matched something that is not a probe, and picking arbitrarily would
    # send console output somewhere unpredictable.
    if len(dev["ttys"]) != 1:
        print(
            f"probe {serial} ({role}) exposes {len(dev['ttys'])} tty nodes "
            f"({dev['ttys'] or 'none'}); expected exactly one",
            file=sys.stderr,
        )
        return 4

    print(f"{role.upper()}_PROBE_SERIAL={serial}")
    print(f"{role.upper()}_TTY={dev['ttys'][0]}")
    print(f"{role.upper()}_PRODUCT={json.dumps(dev['product'])}")
    return 0


def cmd_check() -> int:
    conf = load_conf()
    roles = sorted(
        k[: -len("_PROBE_SERIAL")].lower()
        for k in conf
        if k.endswith("_PROBE_SERIAL")
    )
    if not roles:
        print(f"no roles defined in {CONF}", file=sys.stderr)
        return 2

    devices = discover()
    bad = 0
    for role in roles:
        serial = conf[f"{role.upper()}_PROBE_SERIAL"]
        dev = devices.get(serial)
        if dev is None:
            print(f"FAIL  {role:<8} {serial}  not attached")
            bad += 1
        elif len(dev["ttys"]) != 1:
            print(f"FAIL  {role:<8} {serial}  {len(dev['ttys'])} ttys, expected 1")
            bad += 1
        else:
            print(f"ok    {role:<8} {serial}  {dev['ttys'][0]}  {dev['product']}")

    # Depth guard. If the registry walk stops short, a probe silently reports
    # "no tty" instead of failing, and the console it owns looks absent rather
    # than unreachable. Any /dev/cu.usbmodem* we did not attribute to some USB
    # device is that failure, so name it here rather than debugging a missing
    # console later.
    claimed = {tty for dev in devices.values() for tty in dev["ttys"]}
    orphans = sorted(set(glob.glob("/dev/cu.usbmodem*")) - claimed)
    if orphans:
        print(
            f"FAIL  registry  {len(orphans)} tty node(s) matched to no USB "
            f"device: {', '.join(orphans)} -- raise IOREG_DEPTH"
        )
        bad += 1

    capture = conf.get("CAPTURE_VIDEO_DEVICE")
    if capture:
        listing = subprocess.run(
            ["ffmpeg", "-hide_banner", "-f", "avfoundation",
             "-list_devices", "true", "-i", ""],
            capture_output=True, text=True,
        ).stderr
        # avfoundation only addresses devices by name or index, and indices
        # renumber when any camera appears or disappears, so we match on name.
        if f"] {capture}" in listing:
            print(f"ok    capture   {capture}")
        else:
            print(f"FAIL  capture   {capture}  not offered by avfoundation")
            bad += 1

    return 1 if bad else 0


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    cmd = argv[1]
    if cmd == "list":
        return cmd_list()
    if cmd == "resolve":
        if len(argv) != 3:
            print("usage: devices.py resolve <role>", file=sys.stderr)
            return 2
        return cmd_resolve(argv[2])
    if cmd == "check":
        return cmd_check()
    print(f"unknown command: {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
