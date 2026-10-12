#!/usr/bin/env python3
"""rawhid_test.py [N]: talk to the FM-1's raw HID interface (VIA / Vial's channel) through Linux hidraw.

Finds the hidraw node whose report descriptor starts with usage page 0xFF60, usage 0x61 (QMK raw HID), then:
  1. VIA's id_get_protocol_version (0x01): expects 01 00 0C (protocol 12, the firmware's test responder)
  2. N echo round trips (default 200) of random 32-byte reports: every byte back, the round-trip times
Needs read / write access to /dev/hidraw* (root, or a udev rule). No libraries beyond Python's."""
import glob
import os
import random
import select
import sys
import time

RAW_SIG = bytes.fromhex("0660ff0961")


def find():
    for dev in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        try:
            rd = open(f"{dev}/device/report_descriptor", "rb").read()
        except OSError:
            continue
        if rd.startswith(RAW_SIG):
            return "/dev/" + os.path.basename(dev)
    sys.exit("no raw HID interface (usage page 0xFF60) found")


def xfer(fd, req, timeout=1.0):
    os.write(fd, b"\x00" + req)                 # report ID 0 (none), then 32 bytes
    r, _, _ = select.select([fd], [], [], timeout)
    if not r:
        raise TimeoutError("no reply")
    return os.read(fd, 64)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    path = find()
    fd = os.open(path, os.O_RDWR)
    print(f"raw HID: {path}")
    rep = xfer(fd, bytes([0x01]) + bytes(31))
    ok = rep[:3] == b"\x01\x00\x0c" and len(rep) == 32
    print(f"protocol version: {rep[:3].hex(' ')} ({len(rep)} bytes) {'ok' if ok else 'UNEXPECTED'}")
    bad, times = 0, []
    rnd = random.Random(1)
    for i in range(n):
        req = bytes([0x80 | rnd.randrange(0x7F)]) + bytes(rnd.randrange(256) for _ in range(31))
        t0 = time.perf_counter()
        rep = xfer(fd, req)
        times.append(time.perf_counter() - t0)
        if rep != req:
            bad += 1
            if bad <= 3:
                print(f"  mismatch {i}: sent {req.hex()} got {rep.hex()}")
    times.sort()
    print(f"echo: {n - bad}/{n} exact, round trip median {times[len(times) // 2] * 1000:.2f} ms, "
          f"max {times[-1] * 1000:.2f} ms")
    os.close(fd)
    sys.exit(0 if ok and not bad else 1)


main()
