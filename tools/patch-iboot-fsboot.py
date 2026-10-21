#!/usr/bin/env python3
"""iBoot.autogo -> iBoot.fsboot (J33i 12H847): autoboot /linux.kc even with USB attached.
(0xbd4/0xb06 below: fsboot ignores the boot-path variable.)

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
    # NVRAM carries its own boot-path (not shown by iOS `nvram -p`), which beats the default:
    # fsboot's `r5 = getenv("boot-path")` becomes `r5 = "/linux.kc"` (pool 0xbd4, used only here).
    (0xBD4, bytes.fromhex("c750f39f"), bytes.fromhex("4ad7f39f")),
    (0xB06, bytes.fromhex("18f0a9f8"), bytes.fromhex("00bf00bf")),
    # The kernelcache loader mallocs the compressed size and copies the payload there before
    # decoding into the file buffer. linux.kc is 32 MB of incompressible data: the heap panics
    # ("heap overflow") and iBoot resets. Use free DRAM at 0x88000000 (file buffer 0x84000000
    # + <0x3F00000 below it) and drop the matching free().
    (0x1CA3E, bytes.fromhex("fdf78ffd"), bytes.fromhex("4ff00840")),   # r0 = malloc() -> 0x88000000
    (0x1CA9C, bytes.fromhex("fdf7bafd"), bytes.fromhex("00bf00bf")),   # free(r4) -> nop
    # Hand over like `go` (prepare_and_jump type 0), not as an iOS kernel (type 3): the USB
    # route that boots Linux uses type 0, and type 3 presumably quiesces USB/display for XNU.
    (0xA0E, bytes.fromhex("0320"), bytes.fromhex("0020")),
]

src, dst = (Path(a) for a in sys.argv[1:3])
d = bytearray(src.read_bytes())
for off, old, new in PATCHES:
    if d[off:off + len(old)] != old:
        raise SystemExit(f"{off:#x}: expected {old.hex()}, found {d[off:off + len(old)].hex()}")
    d[off:off + len(new)] = new
dst.write_bytes(d)
print(f"{dst}: {len(PATCHES)} patches")
