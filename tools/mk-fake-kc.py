#!/usr/bin/env python3
"""Wrap the staging bundle as a kernelcache iBoot's fsboot will load (J33i, iOS 8 32-bit).

iBoot RELEASE has no bootx/memboot: fsboot (file -> img3 'krnl' -> complzss -> Mach-O) is
its only way to start a kernel. With the --rsa patch the signature is not checked, so a
Mach-O whose one segment carries the bundle boots Linux with no USB upload.

    [Mach-O header + LC_SEGMENT __TEXT @0x80001000 + LC_UNIXTHREAD][pad to 0x1000][bundle][tramp]

The trampoline sits AFTER the bundle, so the forward copy of the bundle down to 0x80000000
(where its loader is linked) never overwrites the running code. It finds the bundle
PC-relative, so it works whatever vm->phys offset iBoot applies.

LZSS is literal-only (flag 0xff + 8 literals): no search, +12.5% size. The bundle is
mostly compressed data already, so real LZSS would buy little.

  mk-fake-kc.py <staging-bundle.bin> <kernelcache-template.img3> <out.img3>
"""
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
from img3pack import pack_plain  # noqa: E402

VMADDR = 0x80001000   # the real j33i kernel's __TEXT
DEST = 0x80000000     # where linux_boot (bundle offset 0) is linked
HDR = 0x1000

# TRAMP=reset|copyreset (tests): call iBoot's platform_reset (12H847 0x9FF20440) at entry or after
# the copy; with VBUS off a reset brings iOS back, which shows the stub was reached.
TRAMP = __import__("os").environ.get("TRAMP", "")
RESET = "ldr sp, =0x9F000000\n    mov r0, #0\n    ldr r1, =0x9FF20441\n    blx r1\n"

TRAMP_S = """
    .syntax unified
    .arm
    .global _start
_start:
{pre}
    adr   r1, _start
    ldr   r3, size
    sub   r1, r1, r3          @ bundle sits just before the trampoline
    ldr   r2, dest
    mov   r4, r2
1:  ldr   r0, [r1], #4
    str   r0, [r2], #4
    subs  r3, r3, #4
    bne   1b
    ldr   r0, =0x9FF21F75         @ iBoot's cache clean+flush (what autogo calls), for an MMU-on entry
    blx   r0
    mov   r0, #0
    mcr   p15, 0, r0, c7, c5, 0   @ invalidate I-cache
    dsb
    isb
{post}
    bx    r4
    .ltorg
    .align 2
dest: .word {dest:#x}
size: .word {size:#x}
"""


def tramp(size: int) -> bytes:
    with tempfile.TemporaryDirectory() as d:
        s, o, b = (Path(d) / n for n in ("t.S", "t.o", "t.bin"))
        s.write_text(TRAMP_S.format(dest=DEST, size=size, pre=RESET if TRAMP == "reset" else "",
                                     post=RESET if TRAMP == "copyreset" else ""))
        subprocess.run(["clang", "-target", "armv7-none-eabi", "-c", str(s), "-o", str(o)], check=True)
        subprocess.run(["llvm-objcopy" if subprocess.run(["which", "llvm-objcopy"], capture_output=True).returncode == 0
                        else "/opt/homebrew/opt/llvm/bin/llvm-objcopy", "-O", "binary", str(o), str(b)], check=True)
        return b.read_bytes()


def macho(bundle: bytes) -> bytes:
    bundle += b"\0" * (-len(bundle) % 4)
    t = tramp(len(bundle))
    body = bundle + t
    filesize = HDR + len(body)
    vmsize = (filesize + 0xFFF) & ~0xFFF
    entry = VMADDR + HDR + len(bundle)
    seg = struct.pack("<II16sIIIIIIII", 1, 56, b"__TEXT", VMADDR, vmsize, 0, filesize, 7, 5, 0, 0)
    # LC_UNIXTHREAD, ARM_THREAD_STATE (flavor 1, count 17): r0-r12, sp, lr, pc, cpsr
    thr = struct.pack("<IIII", 5, 16 + 17 * 4, 1, 17) + struct.pack("<17I", *([0] * 15 + [entry, 0]))
    cmds = seg + thr
    # mach_header: MH_MAGIC, CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, MH_EXECUTE, ncmds, sizeofcmds, flags
    hdr = struct.pack("<7I", 0xFEEDFACE, 12, 9, 2, 2, len(cmds), 1) + cmds
    return hdr + b"\0" * (HDR - len(hdr)) + body


def lzss_literal(data: bytes) -> bytes:
    out = bytearray()
    for i in range(0, len(data), 8):
        chunk = data[i:i + 8]
        out.append((1 << len(chunk)) - 1)
        out += chunk
    return bytes(out)


def complzss(data: bytes) -> bytes:
    comp = lzss_literal(data)
    hdr = b"complzss" + struct.pack(">IIII", zlib.adler32(data) & 0xFFFFFFFF, len(data), len(comp), 1)
    return hdr + b"\0" * (0x180 - len(hdr)) + comp


def main() -> None:
    bundle, template, out = (Path(a) for a in sys.argv[1:4])
    m = macho(bundle.read_bytes())
    img = pack_plain(template.read_bytes(), complzss(m))
    out.write_bytes(img)
    print(f"{out}: {len(img)} bytes, Mach-O {len(m)} bytes")


if __name__ == "__main__":
    main()
