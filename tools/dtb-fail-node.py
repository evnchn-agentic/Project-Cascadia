#!/usr/bin/env python3
"""Mark DT nodes unavailable inside a staging bundle, in place: status "okay" -> "fail" (same length).

For bisecting which device's probe hangs a boot with no console: a node whose status is "fail"
is skipped by of_platform_populate. Matches node names by prefix (e.g. "gpio@", "usb@6100000").

  dtb-fail-node.py <bundle.bin> <out.bin> <node-name-prefix> [...]
"""
import struct
import sys

FDT_MAGIC, BEGIN, END, PROP, NOP, FEND = 0xD00DFEED, 1, 2, 3, 4, 9

src, dst, *names = sys.argv[1:]
b = bytearray(open(src, "rb").read())
base = b.find(struct.pack(">I", FDT_MAGIC))
while base >= 0:
    total, off_struct, off_strings = struct.unpack_from(">III", b, base + 4)
    if 0 < total < 1 << 20 and b"status" in b[base + off_strings: base + total]:
        break
    base = b.find(struct.pack(">I", FDT_MAGIC), base + 4)
assert base >= 0, "no FDT in bundle"

p, path, hits = base + off_struct, [], 0
while True:
    tok, = struct.unpack_from(">I", b, p); p += 4
    if tok == BEGIN:
        e = b.index(0, p); path.append(b[p:e].decode()); p = (e + 4) & ~3
    elif tok == END:
        path.pop()
    elif tok == PROP:
        ln, nameoff = struct.unpack_from(">II", b, p); p += 8
        e = b.index(0, base + off_strings + nameoff)
        pname = b[base + off_strings + nameoff: e].decode()
        if pname == "status" and path and any(path[-1].startswith(n) for n in names) and b[p:p + 5] == b"okay\0":
            b[p:p + 5] = b"fail\0"; hits += 1
            print("failed", "/".join(path))
        p = (p + ln + 3) & ~3
    elif tok == NOP:
        continue
    elif tok == FEND:
        break
open(dst, "wb").write(b)
print(f"{hits} node(s) marked fail")
