#!/usr/bin/env python3
"""iBoot.autogo -> iBoot.fsboot (J33i 12H847): autoboot /linux.kc even with USB attached.

  0x218e0  the USB-attached check (`bl usb_connected; cmp r0,#1; ... return !auto-boot-usb`)
           -> `movs r0,#0; nop`: never hold in Recovery because of USB.
  0x3d74a  default boot-path "/System/Library/Caches/com.apple.kernelcaches/kernelcache"
           -> "/linux.kc" (on boot-partition 0, the plaintext system partition).

  patch-iboot-fsboot.py <iBoot.autogo> <iBoot.fsboot>
"""
import sys
from pathlib import Path

KC = b"/System/Library/Caches/com.apple.kernelcaches/kernelcache\0"
PATCHES = [
    (0x218E0, bytes.fromhex("04f08cfc"), bytes.fromhex("002000bf")),
    (0x3D74A, KC, b"/linux.kc".ljust(len(KC), b"\0")),
]

src, dst = (Path(a) for a in sys.argv[1:3])
d = bytearray(src.read_bytes())
for off, old, new in PATCHES:
    if d[off:off + len(old)] != old:
        raise SystemExit(f"{off:#x}: expected {old.hex()}, found {d[off:off + len(old)].hex()}")
    d[off:off + len(new)] = new
dst.write_bytes(d)
print(f"{dst}: {len(PATCHES)} patches")
