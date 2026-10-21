#!/usr/bin/env python3
"""One kloader image that runs iBEC's init, then hands over to iBoot without USB (J33i 12H847).

kloader of iBoot straight from iOS hangs ~3 times in 4 (display init is not why); iBEC always
lands, and iBoot sent to iBEC over USB always works. So let iBEC do the init and jump to an
iBoot copy already in RAM:

    [stub][iBEC'][iBoot.fsboot]
    STASH is in /vram (0x9F570000+): iBEC has no display and leaves it alone, but it wipes the
    DRAM below (a stash at 0x80000000 / 0x84000000 / 0x90000000 was gone by the time it jumped).
    stub    PC-relative: copy the appended iBoot to STASH, invalidate the I-cache, jump into iBEC
    iBEC'   iBEC.patched; its Recovery "poweroff" task (pool 0xe48) now runs a cave (0x4090e):
            task_sleep 3 s, then its own prepare_and_jump(0, STASH, 0, 0), the call `go` makes.
            (Jumping from the autoboot path instead, before Recovery is up, hangs.)
    iBoot   self-relocates from STASH to 0x9FF00000, then fsboots /linux.kc

  mk-chain-kloader.py <iBEC.patched> <iBoot.fsboot> <out>
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

BASE = 0x9FF00000
STASH = int(__import__("os").environ.get("STASH", "0x9F600000"), 16)
TASK_PTR, TASK_ORIG = 0xE48, 0x9FF00E69                       # poweroff task function
TASK_SLEEP = 0x1E2BC
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


def t32_mov16(top: bool, rd: int, imm: int) -> bytes:
    """movw / movt rd, #imm16"""
    h1 = 0xF240 | (0x80 if top else 0) | ((imm >> 11) & 1) << 10 | (imm >> 12)
    h2 = ((imm >> 8) & 7) << 12 | rd << 8 | (imm & 0xFF)
    return struct.pack("<HH", h1, h2)


def patch_ibec(d: bytearray) -> bytes:
    if struct.unpack_from("<I", d, TASK_PTR)[0] != TASK_ORIG or any(d[CAVE:CAVE + 48]):
        raise SystemExit("iBEC does not match 12H847 iBEC.patched")
    us = 3_000_000
    cave = t32_mov16(False, 0, us & 0xFFFF) + t32_mov16(True, 0, us >> 16) + bytes.fromhex("0021")
    cave += t32_branch(BASE + CAVE + len(cave), BASE + TASK_SLEEP, True)          # task_sleep(3 s)
    cave += bytes.fromhex("0020") + t32_mov16(False, 1, STASH & 0xFFFF) + t32_mov16(True, 1, STASH >> 16)
    cave += bytes.fromhex("0022" "0023")                                         # r0=0 r1=STASH r2=r3=0
    cave += t32_branch(BASE + CAVE + len(cave), BASE + PREPARE_AND_JUMP, True)
    cave += bytes.fromhex("fee7")                                                # b . (never returns)
    d[CAVE:CAVE + len(cave)] = cave
    struct.pack_into("<I", d, TASK_PTR, BASE + CAVE + 1)
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
