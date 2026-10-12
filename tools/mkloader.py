#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""mkloader.py LOADER.bin OTA.bin: wrap felucca-bsp's update loader image as the package's ota.bin, as
Felucca's tools/build.py ldr_wrap does (hugelton/Felucca, GPL-3.0): an outer JLFS head (32 B), an inner head
(32 B, the load address), then [clen u32][dlen u32][LZ4 block] per 4 KiB."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lz4blk  # noqa: E402  (Felucca's tools/lz4blk.py)

LOADER_LOAD = 0x01C0A800
LOADER_NAME = b"usb_hid_ota.bin"    # the file name the SPL looks for


def crc16(d, c=0):
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
        c &= 0xFFFF
    return c


def ldr_head(dcrc, a, b, attr, name):
    body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, name)
    return struct.pack("<H", crc16(body)) + body


def ldr_wrap(image):
    blocks = bytearray()
    for i in range(0, len(image), 4096):
        raw = image[i:i + 4096]
        c = lz4blk.compress(raw)
        if len(c) > 4096 or lz4blk.decompress(c) != raw:
            raise SystemExit(f"loader: block {i // 4096} does not compress below 4096 B")
        blocks += struct.pack("<II", len(c), len(raw)) + c
    inner = ldr_head(crc16(image), len(image), LOADER_LOAD, 0, LOADER_NAME)
    body = inner + bytes(blocks)
    return ldr_head(crc16(body), 0x20, len(body), 0x41, LOADER_NAME) + body


image = Path(sys.argv[1]).read_bytes()
if not image or len(image) > 0x14000:
    raise SystemExit("loader: image empty or too big")
ota = ldr_wrap(image)
Path(sys.argv[2]).write_bytes(ota)
print(f"loader: image {len(image)} B at {LOADER_LOAD:#x}, ota.bin {len(ota)} B")
