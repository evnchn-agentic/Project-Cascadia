"""Kernelcache helpers for the J33i audio RE (docs/research/j33i-audio.md): segments, va->offset, annotated Thumb disassembly."""
import struct,sys,re
from capstone import *
from capstone.arm import *
import os
KC=os.environ.get('KC_J33I','~/atv3-linux/work/kc.j33i.macho')  # decompressed 12H847 kernelcache
D=open(KC,'rb').read()
SEGS=[]
def _segs(base_off):
    out=[]
    magic,cpu,sub,ft,ncmds,sz,flags=struct.unpack_from('<7I',D,base_off)
    off=base_off+28
    for _ in range(ncmds):
        cmd,cs=struct.unpack_from('<II',D,off)
        if cmd==1:
            name=D[off+8:off+24].rstrip(b'\0').decode()
            va,vs,fo,fs,mp,ip,ns,fl=struct.unpack_from('<8I',D,off+24)
            secs=[]
            so=off+56
            for k in range(ns):
                sn=D[so:so+16].rstrip(b'\0').decode(); sg=D[so+16:so+32].rstrip(b'\0').decode()
                a,s,o=struct.unpack_from('<3I',D,so+32)
                secs.append((sn,a,s,o)); so+=68
            out.append((name,va,vs,fo,fs,secs))
        off+=cs
    return out
SEGS=_segs(0)
def va2off(va):
    for name,sva,vs,fo,fs,_ in SEGS:
        if name=='__PRELINK_TEXT' and sva<=va<sva+fs: return fo+va-sva
    for name,sva,vs,fo,fs,_ in SEGS:
        if sva<=va<sva+fs and fs: return fo+va-sva
    return None
def rd(va,n): o=va2off(va); return D[o:o+n]
def u32(va): return struct.unpack('<I',rd(va,4))[0]
def cstr(va,maxn=256):
    b=rd(va,maxn); return b.split(b'\0')[0].decode('latin1')
def kext_segs(load):
    return _segs(va2off(load))
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB); md.detail=True
class _Bad:
    def __init__(s,a,b): s.address=a; s.mnemonic='.short'; s.op_str='0x%04x'%b; s.operands=[]
def dis(start,end):
    a=start
    while a<end:
        code=rd(a,end-a); last=a
        for ins in md.disasm(code,a):
            yield ins; last=ins.address+ins.size
        if last>=end: break
        yield _Bad(last,struct.unpack('<H',rd(last,2))[0])
        a=last+2
def annotate(start,end,out=sys.stdout):
    regs={}
    for ins in dis(start,end):
        s='%08x: %-8s %s'%(ins.address,ins.mnemonic,ins.op_str)
        note=''
        try:
            ops=ins.operands
            if ins.mnemonic.startswith('movw') and len(ops)==2 and ops[1].type==ARM_OP_IMM:
                regs[ops[0].reg]=ops[1].imm
            elif ins.mnemonic.startswith('movt') and len(ops)==2 and ops[0].reg in regs:
                regs[ops[0].reg]=(regs[ops[0].reg]&0xffff)|(ops[1].imm<<16)
            elif ins.mnemonic in('add','add.w') and len(ops)==2 and ops[1].type==ARM_OP_REG and ops[1].reg==ARM_REG_PC and ops[0].reg in regs:
                v=(regs[ops[0].reg]+ins.address+4)&0xffffffff
                regs[ops[0].reg]=v
                c=None
                try: c=cstr(v,120)
                except Exception: pass
                note=' ; =%08x %r'%(v,c if c and c.isprintable() and len(c)>2 else '')
            elif ins.mnemonic.startswith('ldr') and len(ops)==2 and ops[1].type==ARM_OP_MEM and ops[1].mem.base==ARM_REG_PC:
                a=((ins.address+4)&~3)+ops[1].mem.disp
                v=u32(a); note=' ; [%08x]=%08x'%(a,v)
                regs[ops[0].reg]=v
            elif ins.mnemonic in ('mov','movs','mov.w','movs.w') and len(ops)==2 and ops[1].type==ARM_OP_IMM:
                regs[ops[0].reg]=ops[1].imm
            else:
                if len(ops)>0 and ops[0].type==ARM_OP_REG and ins.mnemonic not in ('str','str.w','strb','strh','cmp','tst','push','pop','b','bl','blx','bx','cbz','cbnz','it','ite','itt','itete'):
                    regs.pop(ops[0].reg,None)
            if ins.mnemonic in ('bl','blx'): 
                for r in (ARM_REG_R0,ARM_REG_R1,ARM_REG_R2,ARM_REG_R3,ARM_REG_R12): regs.pop(r,None)
        except Exception as e: pass
        print(s+note,file=out)
