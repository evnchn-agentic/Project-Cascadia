import struct,sys
from capstone import *
import os
D=open(os.environ.get('KC841', 'kc841.macho'),'rb').read()
SEGS=[]
off=28
for _ in range(struct.unpack_from('<I',D,16)[0]):
    cmd,csz=struct.unpack_from('<II',D,off)
    if cmd==1:
        nm=D[off+8:off+24].split(b'\0')[0].decode()
        va,vs,fo,fs=struct.unpack_from('<IIII',D,off+24); SEGS.append((nm,va,vs,fo,fs))
    off+=csz
def va2off(va):
    for nm,v,vs,fo,fs in SEGS:
        if v<=va<v+vs and fo+(va-v)<fo+fs: return fo+(va-v)
    return None
def off2va(o):
    for nm,v,vs,fo,fs in SEGS:
        if fo<=o<fo+fs: return v+(o-fo)
    return None
def find_kext(cstr=b'SGXDriver543\0'):
    """__TEXT address of the prelinked kext whose __cstring holds CSTR (the
    GPU kext by default), or None: every prelinked kext is a Mach-O on a page
    boundary in __PRELINK_TEXT (as scripts/extract-sgx-firmware.py finds it)"""
    pt = [s for s in SEGS if s[0] == '__PRELINK_TEXT']
    if not pt: return None
    _, pva, pvs, pfo, pfs = pt[0]
    for o in range(pfo, pfo + pfs - 28, 0x1000):
        if struct.unpack_from('<I', D, o)[0] != 0xfeedface: continue
        text, off = None, o + 28
        for _ in range(struct.unpack_from('<I', D, o + 16)[0]):
            cmd, csz = struct.unpack_from('<II', D, off)
            if cmd == 1 and D[off+8:off+24].split(b'\0')[0] == b'__TEXT':
                text = struct.unpack_from('<I', D, off + 24)[0]
                for k in range(struct.unpack_from('<I', D, off + 48)[0]):
                    s = off + 56 + 68 * k
                    if D[s:s+16].split(b'\0')[0] == b'__cstring':
                        a, n = struct.unpack_from('<II', D, s + 32)
                        c = va2off(a)
                        if c is not None and cstr in D[c:c + n]: return text
            off += csz
    return None
def find_str(s):
    b=s.encode() if isinstance(s,str) else s; out=[]; i=0
    while True:
        i=D.find(b,i)
        if i<0: break
        out.append((i,off2va(i))); i+=1
    return out
def find_ptrs(va):  # 32-bit LE pointers to va, anywhere
    t=struct.pack('<I',va); out=[]; i=0
    while True:
        i=D.find(t,i)
        if i<0: break
        out.append((i,off2va(i))); i+=1
    return out
def dis(va,n=40,thumb=True):
    o=va2off(va); code=D[o:o+n*4]
    md=Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM); md.detail=False
    for ins in md.disasm(code, va):
        print(f"  {ins.address:08x}: {ins.mnemonic:8s} {ins.op_str}")
        n-=1
        if n<=0: break
if __name__=='__main__':
    cmd=sys.argv[1]
    if cmd=='str':
        for o,va in find_str(sys.argv[2]): print(f"  off=0x{o:x} va=0x{va and va or 0:08x} : {sys.argv[2]}")
    elif cmd=='ptr':
        for o,va in find_ptrs(int(sys.argv[2],16)): print(f"  ptr@ off=0x{o:x} va=0x{va and va or 0:08x}")
    elif cmd=='dis':
        dis(int(sys.argv[2],16), int(sys.argv[3]) if len(sys.argv)>3 else 40, 'arm' not in sys.argv)
