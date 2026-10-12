#!/usr/bin/env python3
"""mkbin.py TOOLCHAIN APP.elf OUT.bin: the app image as Felucca's tools/build.py makes it (.text from
0x02000120, then .ram_text and .data at their load addresses; fm1_cstart copies both), its checks (the entry
stub, no calls in .ram_text: it runs with the flash off), and the memory report against firmware/app.ld."""
import re
import subprocess
import sys
from pathlib import Path

tc, elf, dest = sys.argv[1:]
od = f"{tc}/common/bin/objdump"
oc = f"{tc}/common/bin/objcopy"
syms = subprocess.run([od, "-t", elf], capture_output=True, text=True, check=True).stdout


def sym(name):
    m = re.search(r"^([0-9a-f]+) .*\s" + re.escape(name) + r"$", syms, re.M)
    if not m:
        sys.exit(f"mkbin: {name} not in {elf}")
    return int(m.group(1), 16)


out = Path(dest).parent
for sect, f in ((".text", "text.bin"), (".ram_text", "ramtext.bin"), (".data", "data.bin")):
    subprocess.run([oc, "-O", "binary", "-j", sect, elf, str(out / f)], check=True)
APP = 0x02000120
assert sym("_start") == APP, hex(sym("_start"))
img = bytearray((out / "text.bin").read_bytes())
for f, lname in (("ramtext.bin", "_rt_load"), ("data.bin", "_data_load")):
    load, blob = sym(lname), (out / f).read_bytes()
    if load % 4:
        sys.exit(f"mkbin: {lname} {load:#x} is not word aligned")
    if blob:
        if load - APP < len(img):
            sys.exit(f"mkbin: {lname} overlaps the image")
        img += b"\xff" * (load - APP - len(img)) + blob
img += b"\xff" * (-len(img) % 4)
if img[:4] != bytes.fromhex("04818000"):
    sys.exit(f"mkbin: the image starts with {img[:4].hex()}, not the entry stub")
rt = subprocess.run([od, "-d", "-j", ".ram_text", elf], capture_output=True, text=True, check=True).stdout
rt_calls = [ln for ln in rt.splitlines() if re.search(r"\bcall\b", ln)]
if rt_calls:
    sys.exit(f"mkbin: .ram_text calls out (it runs with the flash off): {rt_calls[:3]}")
rt_n = len(re.findall(r"^\s+[0-9a-f]+:", rt, re.M))
Path(dest).write_bytes(img)

xip = 0x8DFBC
ram = sym("_bss_end") - sym("_data_start")
pool = sym("_pool_end") - sym("_pool_start")
print(f"{Path(dest).name}: {len(img)} B of {xip} XIP ({100 * len(img) / xip:.1f} %), "
      f"RAM .data+.bss {ram} of {96 * 1024}, pool {pool} of {0x54000}, .ram_text {rt_n} insns, no calls")
if len(img) > xip or ram > 96 * 1024 or pool > 0x54000:
    sys.exit("mkbin: over a region")
