#!/usr/bin/env python3
"""vialcli.py: talk VIA / Vial to the FM-1 over Linux hidraw (finds the raw HID interface, usage page 0xFF60).

  vialcli.py info                       protocols, layers, keyboard id
  vialcli.py get LAYER ROW COL          a keycode (hex)
  vialcli.py set LAYER ROW COL KEYCODE  set one (KEYCODE hex, e.g. 0x001D for KC_Z)
  vialcli.py enc LAYER INDEX            an encoder's CCW / CW keycodes
  vialcli.py setenc LAYER INDEX CCW CW  set an encoder (keycodes hex; INDEX 0 SELECT 1 ALGORITHM 2..5 KNOB1..4 6 PRESETS)
  vialcli.py dump FILE                  the keymap buffer (all layers) to FILE
  vialcli.py status                     the flash store: saves, errors, which copy loaded at boot
  vialcli.py unlock                     Vial's unlock: hold OCT- + OCT+ on the FM-1 while it counts down
  vialcli.py macro N TEXT               macro N = TEXT (\\n: Enter), the others kept (needs unlock)
Needs read / write access to the hidraw node (root, or Vial's udev rule)."""
import glob
import os
import select
import sys


def find():
    for dev in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        try:
            if open(f"{dev}/device/report_descriptor", "rb").read().startswith(bytes.fromhex("0660ff0961")):
                return "/dev/" + os.path.basename(dev)
        except OSError:
            pass
    sys.exit("no raw HID interface (usage page 0xFF60) found")


fd = os.open(find(), os.O_RDWR)


def xfer(*req):
    msg = bytes(req) + bytes(32 - len(req))
    os.write(fd, b"\x00" + msg)
    if not select.select([fd], [], [], 1.0)[0]:
        sys.exit("no reply")
    return os.read(fd, 64)


cmd = sys.argv[1] if len(sys.argv) > 1 else "info"
a = [int(x, 0) for x in sys.argv[2:]] if cmd not in ("dump", "macro") else []
if cmd == "info":
    r = xfer(0x01)
    print(f"VIA protocol {r[1] << 8 | r[2]}")
    r = xfer(0xFE, 0x00)
    print(f"Vial protocol {int.from_bytes(r[0:4], 'little')}, keyboard id {r[4:12].hex()}")
    print(f"layers {xfer(0x11)[1]}")
elif cmd == "get":
    r = xfer(0x04, a[0], a[1], a[2])
    print(f"0x{r[4] << 8 | r[5]:04X}")
elif cmd == "set":
    xfer(0x05, a[0], a[1], a[2], a[3] >> 8, a[3] & 0xFF)
    r = xfer(0x04, a[0], a[1], a[2])
    print(f"set: now 0x{r[4] << 8 | r[5]:04X}")
elif cmd == "enc":
    r = xfer(0xFE, 0x03, a[0], a[1])
    print(f"CCW 0x{r[0] << 8 | r[1]:04X}  CW 0x{r[2] << 8 | r[3]:04X}")
elif cmd == "setenc":
    for d, kc in ((0, a[2]), (1, a[3])):
        xfer(0xFE, 0x04, a[0], a[1], d, kc >> 8, kc & 0xFF)
    r = xfer(0xFE, 0x03, a[0], a[1])
    print(f"set: CCW 0x{r[0] << 8 | r[1]:04X}  CW 0x{r[2] << 8 | r[3]:04X}")
elif cmd == "dump":
    layers = xfer(0x11)[1]
    size, out = layers * 3 * 16 * 2, b""
    for ofs in range(0, size, 28):
        n = min(28, size - ofs)
        out += xfer(0x12, ofs >> 8, ofs & 0xFF, n)[4:4 + n]
    open(sys.argv[2], "wb").write(out)
    print(f"{len(out)} bytes to {sys.argv[2]}")
elif cmd == "status":
    r = xfer(0x02, 0x80)
    if r[0] != 0x02:
        sys.exit("no status (older firmware)")
    v = [int.from_bytes(r[2 + 4 * i:6 + 4 * i], "little") for i in range(6)]
    print(f"saves {v[0]}, errors {v[1]}, loaded at boot: {['defaults', 'copy A', 'copy B'][v[2]]}, seq {v[3]}, "
          f"slot {v[4]:#07x}, unsaved changes {v[5]}")
elif cmd == "unlock":
    import time
    r = xfer(0xFE, 0x05)
    if r[0]:
        print("already unlocked")
        sys.exit(0)
    xfer(0xFE, 0x06)
    print("hold OCT- + OCT+ on the FM-1 ...")
    for _ in range(1200):                    # ~3 minutes to get to the FM-1
        time.sleep(0.15)
        r = xfer(0xFE, 0x07)
        print(f"\r  {r[2]:2d} to go ", end="", flush=True)
        if r[0]:
            print("\nunlocked")
            break
    else:
        sys.exit("\nnot unlocked")
elif cmd == "macro":
    n, text = int(sys.argv[2]), sys.argv[3].replace("\\n", "\n").encode()
    count, size = xfer(0x0C)[1], xfer(0x0D)[1] << 8 | xfer(0x0D)[2]
    buf = b""
    for ofs in range(0, size, 28):
        k = min(28, size - ofs)
        buf += xfer(0x0E, ofs >> 8, ofs & 0xFF, k)[4:4 + k]
    macros = (buf.split(b"\x00") + [b""] * count)[:count]
    macros[n] = text
    new = b"\x00".join(macros) + b"\x00"
    new += bytes(size - len(new))
    for ofs in range(0, size, 28):
        k = min(28, size - ofs)
        xfer(0x0F, ofs >> 8, ofs & 0xFF, k, *new[ofs:ofs + k])
    back = b""                              # a locked FM-1 answers a write unchanged: read back to know
    for ofs in range(0, size, 28):
        k = min(28, size - ofs)
        back += xfer(0x0E, ofs >> 8, ofs & 0xFF, k)[4:4 + k]
    if back != new:
        sys.exit("not written: unlock first (vialcli.py unlock)")
    print(f"macro {n} = {text!r} ({len(new.rstrip(bytes(1)))} of {size} bytes used)")
else:
    sys.exit(__doc__)
