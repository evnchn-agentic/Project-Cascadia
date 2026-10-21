#!/usr/bin/env python3
"""One kloader image that runs iBEC's init, then hands over to iBoot without USB (J33i 12H847).

kloader of iBoot straight from iOS hangs ~3 times in 4 (display init is not why); iBEC always
lands, and iBoot sent to iBEC over USB always works. So let iBEC do the init and jump to an
iBoot copy already in RAM:

    [stub][iBEC'][iBoot.fsboot]
    stub    PC-relative: copy the appended iBoot to STASH, invalidate the I-cache, jump into iBEC
    iBEC'   iBEC.patched; where autoboot gives up ("boot-command 'fsboot' not supported", 0xdc2)
            it calls its own prepare_and_jump(0, STASH, 0, 0) instead (code cave 0x4090e)
    iBoot   self-relocates from STASH to 0x9FF00000, then fsboots /linux.kc

  mk-chain-kloader.py <iBEC.patched> <iBoot.fsboot> <out>
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

BASE = 0x9FF00000
STASH = 0x90000000
HOOK, HOOK_ORIG = 0xDC2, bytes.fromhex("1f4821462ff01ffe")   # ldr r0,=fmt; mov r1,r4; bl printf
CAVE = 0x4090E
PREPARE_AND_JUMP = 0x1D6C8
STUB_LEN = 0x100


def t32_branch(src: int, dst: int, link: bool) -> bytes:
    off = dst - (src + 4)
    assert -(1 << 24) <= off < (1 << 24) and off % 2 == 0
    s = (off >> 24) & 1
    i1, i2 = (off >> 23) & 1, (off >> 22) & 1
    j1, j2 = (1 - i1) ^ s, (1 - i2) ^ s
    h1 = 0xF000 | (s << 10) | ((off >> 12) & 0x3FF)
    h2 = (0xD000 if link else 0x9000) | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7FF)
    return struct.pack("<HH", h1, h2)


def patch_ibec(d: bytearray) -> bytes:
    if d[HOOK:HOOK + 8] != HOOK_ORIG or any(d[CAVE:CAVE + 32]):
        raise SystemExit("iBEC does not match 12H847 iBEC.patched")
    cave = (bytes.fromhex("0020" "40f20001" "c9f20001" "0022" "0023")   # r0=0 r1=STASH r2=r3=0
            + t32_branch(BASE + CAVE + 14, BASE + PREPARE_AND_JUMP, True)
            + bytes.fromhex("fee7"))                                   # b . (never returns)
    d[CAVE:CAVE + len(cave)] = cave
    d[HOOK:HOOK + 4] = t32_branch(BASE + HOOK, BASE + CAVE, False)
    return bytes(d)


STUB_S = """
    .syntax unified
    .arm
_start:
    adr   r0, _start
    add   r5, r0, #{stub:#x}      @ iBEC entry
    ldr   r1, iboot_off
    add   r1, r0, r1
    ldr   r2, stash
    ldr   r3, size
1:  ldr   r4, [r1], #4
    str   r4, [r2], #4
    subs  r3, r3, #4
    bne   1b
    mov   r0, #0
    mcr   p15, 0, r0, c7, c5, 0
    dsb
    isb
    bx    r5
    .align 2
iboot_off: .word {iboot_off:#x}
stash:     .word {stash:#x}
size:      .word {size:#x}
"""


def stub(iboot_off: int, size: int) -> bytes:
    with tempfile.TemporaryDirectory() as t:
        s, o, b = (Path(t) / n for n in ("s.S", "s.o", "s.bin"))
        s.write_text(STUB_S.format(stub=STUB_LEN, iboot_off=iboot_off, stash=STASH, size=size))
        subprocess.run(["clang", "-target", "armv7-none-eabi", "-c", str(s), "-o", str(o)], check=True)
        subprocess.run(["/opt/homebrew/opt/llvm/bin/llvm-objcopy", "-O", "binary", str(o), str(b)], check=True)
        raw = b.read_bytes()
    assert len(raw) <= STUB_LEN
    return raw.ljust(STUB_LEN, b"\0")


def main() -> None:
    ibec, iboot, out = (Path(a) for a in sys.argv[1:4])
    e = patch_ibec(bytearray(ibec.read_bytes()))
    e += b"\0" * (-len(e) % 16)
    i = iboot.read_bytes()
    i += b"\0" * (-len(i) % 4)
    blob = stub(STUB_LEN + len(e), len(i)) + e + i
    out.write_bytes(blob)
    print(f"{out}: {len(blob)} bytes (iBEC' {len(e)}, iBoot {len(i)} -> {STASH:#x})")


if __name__ == "__main__":
    main()
