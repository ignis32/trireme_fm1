#!/usr/bin/env python3
"""pkg_image.py PACKAGE.fwsc OUT.img: the logical image fm1_install.py serves to the device, made and checked by
fm1_install.py's own load_package (felucca-bsp tools/); prints the package identity."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "third_party" / "felucca-bsp" / "tools"))
import fm1_install  # noqa: E402

product, image = fm1_install.load_package(sys.argv[1], False)
Path(sys.argv[2]).write_bytes(image)
print(product)
