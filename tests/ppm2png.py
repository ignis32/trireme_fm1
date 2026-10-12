#!/usr/bin/env python3
"""ppm2png.py DIR: every DIR/*.ppm as a PNG beside it, scaled x2 (to look at the screens; needs Pillow)."""
import pathlib
import sys

from PIL import Image

for p in sorted(pathlib.Path(sys.argv[1]).glob("*.ppm")):
    im = Image.open(p)
    im.resize((im.width * 2, im.height * 2), Image.NEAREST).save(p.with_suffix(".png"))
    print(p.with_suffix(".png"))
