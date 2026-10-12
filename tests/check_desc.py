#!/usr/bin/env python3
"""check_desc.py [--raw] OURS.txt BSP.txt: check the descriptors tests/desc_dump.c printed.
--raw: the raw flavour (no console; a raw HID interface after the keyboard: VIA / Vial's usage page 0xFF60,
usage 0x61, 32-byte input and output, interrupt IN + OUT).

Per variant (console on / off x keyboard on / off): the device descriptor, and the configuration walked
descriptor by descriptor (lengths add up to wTotalLength, bNumInterfaces = the interfaces present, numbered 0..n-1,
endpoint addresses unique, an IAD's range inside the configuration). With the keyboard: one HID interface (boot
keyboard), its HID descriptor's report length = the report descriptor's, an interrupt IN endpoint of 8 bytes; the
report descriptor parsed: a 64-bit input report (modifiers, reserved, 6 keys up to usage 255) and an 8-bit output
report (5 LEDs + padding). With the keyboard off: byte for byte felucca-bsp's descriptors (BSP.txt)."""
import sys


def load(path):
    out, cur = {}, None
    for ln in open(path):
        w = ln.split()
        if not w:
            continue
        if w[0] == "variant":
            cur = tuple(int(x.split("=")[1]) for x in w[1:])
            out[cur] = {}
        elif w[0] in ("device", "config", "product", "serial", "report", "raw_report", "media_report") \
                and cur is not None:
            key = w[0]
            if key in ("report", "raw_report", "media_report"):
                out[key] = bytes(int(x, 16) for x in w[1:])
            else:
                out[cur][key] = bytes(int(x, 16) for x in w[1:])
        elif w[0] == "fails":
            out["fails"] = int(w[1])
        elif w[0] == "FAIL":
            print(ln.rstrip())
    return out


errors = []


def err(msg):
    errors.append(msg)
    print("ERROR", msg)


def walk(cfg):
    i, items = 0, []
    while i < len(cfg):
        n = cfg[i]
        if n < 2 or i + n > len(cfg):
            err(f"descriptor at {i}: bad length {n}")
            break
        items.append(cfg[i:i + n])
        i += n
    return items


def parse_report(rd):
    """HID 1.11 6.2.2 short items: returns (input bits, output bits, max usage / logical max of the key array)"""
    i, g, bits = 0, {"size": 0, "count": 0, "lmax": 0}, {"in": 0, "out": 0}
    local, arrays, depth = {}, [], 0
    while i < len(rd):
        b = rd[i]
        size = (0, 1, 2, 4)[b & 3]
        tag = b & 0xFC
        data = int.from_bytes(rd[i + 1:i + 1 + size], "little")
        i += 1 + size
        if tag == 0x74:
            g["size"] = data
        elif tag == 0x94:
            g["count"] = data
        elif tag == 0x24:
            g["lmax"] = data if size != 1 or data < 0x80 else data - 256
        elif tag == 0x28:
            local["umax"] = data
        elif tag in (0x80, 0x90):
            bits["in" if tag == 0x80 else "out"] += g["size"] * g["count"]
            if tag == 0x80 and not data & 3:                  # an array of data (not constant padding)
                arrays.append((local.get("umax"), g["lmax"], g["count"], g["size"]))
            local = {}
        elif tag == 0xA0:
            depth += 1
        elif tag == 0xC0:
            depth -= 1
    return bits, arrays, depth


def check(v, d, report):
    cdc, hid = v
    dev, cfg = d["device"], d["config"]
    if len(dev) != 18 or dev[0] != 18 or dev[1] != 1:
        err(f"{v}: device descriptor {dev.hex()}")
    bcd_hi = dev[13]
    if bcd_hi != (4 if hid else 3):
        err(f"{v}: bcdDevice high byte {bcd_hi:#x}")
    items = walk(cfg)
    total = int.from_bytes(cfg[2:4], "little")
    if total != len(cfg):
        err(f"{v}: wTotalLength {total}, got {len(cfg)}")
    ifs, eps, hid_ifs = set(), set(), []
    last_if = None
    for k, it in enumerate(items):
        t = it[1]
        if t == 4:
            last_if = it
            if it[3] == 0:
                ifs.add(it[2])
            if it[5] == 3:
                hid_ifs.append(k)
        elif t == 5:
            if it[2] in eps:
                err(f"{v}: endpoint {it[2]:#x} twice")
            eps.add(it[2])
        elif t == 0x0B:
            if it[2] + it[3] > cfg[4]:
                err(f"{v}: IAD {it[2]}+{it[3]} past {cfg[4]} interfaces")
    if cfg[4] != len(ifs) or ifs != set(range(len(ifs))):
        err(f"{v}: bNumInterfaces {cfg[4]}, interfaces {sorted(ifs)}")
    if not hid:
        if hid_ifs:
            err(f"{v}: a HID interface with the keyboard off")
        return
    if len(hid_ifs) != (3 if RAW else 1):
        err(f"{v}: {len(hid_ifs)} HID interfaces")
        return
    if RAW:
        check_raw(v, items, hid_ifs[1], ifs)
        check_media(v, items, hid_ifs[2], ifs)
        ser = d.get("serial", b"")[2:].decode("utf-16-le", "replace")
        if dev[16] != 3 or ser != "vial:f64c2b3c":
            err(f"{v}: iSerialNumber {dev[16]}, string {ser!r}: want 3, 'vial:f64c2b3c' (Vial finds its keyboards by it)")
        else:
            print(f"ok {v}: serial number string 'vial:f64c2b3c'")
    k = hid_ifs[0]
    itf, hd, ep = items[k], items[k + 1], items[k + 2]
    if itf[2] != max(ifs) - (2 if RAW else 0):
        err(f"{v}: the keyboard interface {itf[2]} is not where expected")
    if (itf[4], itf[5], itf[6], itf[7]) != (1, 3, 1, 1):
        err(f"{v}: HID interface {itf.hex()}: want 1 endpoint, class 3, boot subclass, keyboard protocol")
    if hd[1] != 0x21 or int.from_bytes(hd[7:9], "little") != len(report) or hd[6] != 0x22:
        err(f"{v}: HID descriptor {hd.hex()} (report descriptor {len(report)} bytes)")
    if ep[1] != 5 or not ep[2] & 0x80 or ep[3] & 3 != 3 or int.from_bytes(ep[4:6], "little") != 8:
        err(f"{v}: HID endpoint {ep.hex()}: want interrupt IN, 8 bytes")
    print(f"ok {v}: {len(cfg)} B config, {cfg[4]} interfaces, HID interface {itf[2]} on EP {ep[2]:#04x}, "
          f"bInterval {ep[6]}")


def check_raw(v, items, k, ifs):
    itf, hd, e1, e2 = items[k], items[k + 1], items[k + 2], items[k + 3]
    rr = ours.get("raw_report", b"")
    if itf[2] != max(ifs) - 1 or (itf[4], itf[5], itf[6], itf[7]) != (2, 3, 0, 0):
        err(f"{v}: raw interface {itf.hex()}: want the last, 2 endpoints, class 3, no subclass / protocol")
    if hd[1] != 0x21 or int.from_bytes(hd[7:9], "little") != len(rr):
        err(f"{v}: raw HID descriptor {hd.hex()} (report descriptor {len(rr)} bytes)")
    dirs = sorted(e[2] & 0x80 for e in (e1, e2))
    if dirs != [0, 0x80] or any(e[1] != 5 or e[3] & 3 != 3 or int.from_bytes(e[4:6], "little") != 32
                                for e in (e1, e2)) or (e1[2] & 15) != (e2[2] & 15):
        err(f"{v}: raw endpoints {e1.hex()} {e2.hex()}: want interrupt IN + OUT, 32 bytes, one endpoint number")
    # QMK raw HID: usage page 0xFF60 (06 60 FF), usage 0x61 (09 61), 32-byte input and output
    bits, arrays, depth = parse_report(rr)
    if not rr.startswith(bytes.fromhex("0660ff0961a101")) or bits != {"in": 256, "out": 256} or depth:
        err(f"raw report descriptor {rr.hex()}: {bits}")
    else:
        print(f"ok {v}: raw HID interface {itf[2]} on EP {e1[2] & 15}, usage page 0xFF60 / 0x61, 32 + 32 bytes")


def check_media(v, items, k, ifs):
    itf, hd, ep = items[k], items[k + 1], items[k + 2]
    mr = ours.get("media_report", b"")
    if itf[2] != max(ifs) or (itf[4], itf[5], itf[6], itf[7]) != (1, 3, 0, 0):
        err(f"{v}: media interface {itf.hex()}: want the last, 1 endpoint, class 3, no boot")
    if hd[1] != 0x21 or int.from_bytes(hd[7:9], "little") != len(mr):
        err(f"{v}: media HID descriptor {hd.hex()} (report descriptor {len(mr)} bytes)")
    if ep[1] != 5 or ep[2] != 0x82 or ep[3] & 3 != 3 or int.from_bytes(ep[4:6], "little") != 8:
        err(f"{v}: media endpoint {ep.hex()}: want EP2 IN interrupt, 8 bytes")
    # report IDs 1 mouse (8 buttons + X Y wheel + pan = 40 bits), 2 system (16), 3 consumer (16); depth back to 0
    ids, i, bits, cur, depth = [], 0, {}, None, 0
    g = {"size": 0, "count": 0}
    while i < len(mr):
        b = mr[i]
        size = (0, 1, 2, 4)[b & 3]
        tag = b & 0xFC
        data = int.from_bytes(mr[i + 1:i + 1 + size], "little")
        i += 1 + size
        if tag == 0x84:
            cur = data
            ids.append(data)
        elif tag == 0x74:
            g["size"] = data
        elif tag == 0x94:
            g["count"] = data
        elif tag == 0x80:
            bits[cur] = bits.get(cur, 0) + g["size"] * g["count"]
        elif tag == 0xA0:
            depth += 1
        elif tag == 0xC0:
            depth -= 1
    if ids != [1, 2, 3] or bits != {1: 40, 2: 16, 3: 16} or depth:
        err(f"media report descriptor: ids {ids}, input bits {bits}, depth {depth}")
    else:
        print(f"ok {v}: media interface {itf[2]} on EP 0x82: mouse (ID 1), system (ID 2), consumer (ID 3)")


RAW = sys.argv[1] == "--raw"
if RAW:
    sys.argv.pop(1)
ours, bsp = load(sys.argv[1]), load(sys.argv[2])
report = ours.get("report", b"")
bits, arrays, depth = parse_report(report)
if bits != {"in": 64, "out": 8} or depth != 0:
    err(f"report descriptor: input {bits['in']} bits, output {bits['out']} bits, depth {depth}")
if arrays != [(255, 255, 6, 8)]:
    err(f"report descriptor: key array {arrays} (want usage max 255, logical max 255, 6 x 8 bits)")
else:
    print(f"ok report descriptor: {len(report)} B, input 64 bits (boot layout), output 8 bits, keys 0..255")
for v in ([(0, 0), (0, 1)] if RAW else [(0, 0), (1, 0), (0, 1), (1, 1)]):
    if v not in ours:
        err(f"{v}: missing")
        continue
    check(v, ours[v], report)
    if not v[1]:
        if ours[v] != bsp.get(v):
            err(f"{v}: differs from felucca-bsp's descriptors")
        else:
            print(f"ok {v}: byte for byte felucca-bsp's")
if ours.get("fails", 1) or bsp.get("fails", 1):
    err(f"desc_dump assertions failed: {ours.get('fails')} (ours), {bsp.get('fails')} (bsp)")
print("FAILED" if errors else "all descriptor checks passed")
sys.exit(1 if errors else 0)
