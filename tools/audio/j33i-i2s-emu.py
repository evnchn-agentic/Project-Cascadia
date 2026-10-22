"""Run J33i (12H847) AppleSamsungI2SController configure/start in unicorn.
vtable 0x80a220c8: +0x344 validate 0x80a20a94, +0x348 configure 0x80a20d78,
+0x34c slave-configure 0x80a21230, +0x350 start 0x80a21408, +0x354 stop 0x80a21460."""
import struct,sys
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import j33i_kc as kc
from unicorn import *
from unicorn.arm_const import *
VT=0x80a220c8
CONFIGURE=0x80a20d78; START=0x80a21408; POWER=0x80a20968
HEAP,HEAP_SZ=0x10000000,0x01000000; STACK,STACK_SZ=0x20000000,0x00100000
FAKE,FAKE_SZ=0x30000000,0x00100000; MMIO=0x40000000; SENT=0x3ffff000
uc=Uc(UC_ARCH_ARM,UC_MODE_THUMB)
for name,va,vs,fo,fs,_ in kc.SEGS:
    if vs==0: continue
    b,e=va&~0xfff,(va+vs+0xfff)&~0xfff
    try: uc.mem_map(b,e-b,UC_PROT_ALL)
    except UcError: pass
    if fs: uc.mem_write(va,kc.D[fo:fo+min(fs,vs)])
for a,s in ((HEAP,HEAP_SZ),(STACK,STACK_SZ),(FAKE,FAKE_SZ),(MMIO,0x10000),(SENT&~0xfff,0x1000)): uc.mem_map(a,s)
top=[HEAP]
def alloc(n):
    p=top[0]; top[0]=(p+n+15)&~15; uc.mem_write(p,b'\0'*n); return p
def u32(a): return struct.unpack('<I',uc.mem_read(a,4))[0]
def w32(a,v): uc.mem_write(a,struct.pack('<I',v&0xffffffff))
fakes=[]; log=[]
NCLK=int(sys.argv[1]) if len(sys.argv)>1 else 24000000
def fake(name):
    n=len(fakes); fakes.append(name); vt=alloc(0x1000)
    for i in range(0x400): w32(vt+4*i,(FAKE+n*0x1000+4*i)|1)
    o=alloc(0x40); w32(o,vt); return o
def on_fake(mu,addr,size,_):
    n,slot=(addr-FAKE)>>12,(addr-FAKE)&0xfff
    r=[mu.reg_read(x) for x in (UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
    extra=''; ret0,ret1=1,0
    if fakes[n].endswith('mclk') and slot==0x3c: extra='  hz=%d'%u32(r[0])
    if fakes[n].startswith('provider') and slot==0x34c: ret0,ret1=NCLK,0; extra=' (nclk -> %d)'%NCLK
    log.append('call %s+%#x(%#x,%#x,%#x)%s'%(fakes[n],slot,r[0],r[1],r[2],extra))
    mu.reg_write(UC_ARM_REG_R0,ret0); mu.reg_write(UC_ARM_REG_R1,ret1)
    mu.reg_write(UC_ARM_REG_PC,mu.reg_read(UC_ARM_REG_LR))
def on_kernel(mu,addr,size,_):
    lr=mu.reg_read(UC_ARM_REG_LR); log.append('kernel %#x from %#x'%(addr,lr))
    mu.reg_write(UC_ARM_REG_R0,0); mu.reg_write(UC_ARM_REG_PC,lr)
state={}
def on_read(mu,acc,addr,size,v,_):
    off=addr-MMIO; v=state.get(off,0)
    if (off&0xfff)==0: v|=2   # CLKCON 'stopped' bit
    w32(addr,v); log.append('  read  %s+%#05x -> %08x'%(win(off),off&0xfff,v))
def on_write(mu,acc,addr,size,v,_):
    off=addr-MMIO; state[off]=v&0xffffffff; log.append('  write %s+%#05x = %08x'%(win(off),off&0xfff,v&0xffffffff))
WN={}
def win(off): return WN.get(off&~0xfff,'?')
uc.hook_add(UC_HOOK_CODE,on_fake,begin=FAKE,end=FAKE+FAKE_SZ-1)
uc.hook_add(UC_HOOK_CODE,on_kernel,begin=0x80001000,end=0x80392000)
uc.hook_add(UC_HOOK_MEM_READ,on_read,begin=MMIO,end=MMIO+0xffff)
uc.hook_add(UC_HOOK_MEM_WRITE,on_write,begin=MMIO,end=MMIO+0xffff)
uc.mem_write(SENT,bytes.fromhex('00bf00bfe047fee7'))
def call(fn,args=()):
    uc.reg_write(UC_ARM_REG_SP,STACK+STACK_SZ-0x1000)
    for r,a in zip((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3),args): uc.reg_write(r,a&0xffffffff)
    uc.reg_write(UC_ARM_REG_R12,fn|1)
    try: uc.emu_start(SENT|1,SENT+6,count=2_000_000)
    except UcError as e:
        print('\n'.join(log)); sys.exit('stop: %s pc %#x lr %#x'%(e,uc.reg_read(UC_ARM_REG_PC),uc.reg_read(UC_ARM_REG_LR)))
    return uc.reg_read(UC_ARM_REG_R0)
def new_i2s(name,idx,master):
    o=alloc(0x100); w32(o,VT)
    w32(o+0x7c,fake('provider_'+name)); w32(o+0x84,MMIO+idx*0x1000); WN[idx*0x1000]=name
    w32(o+0x88,3)                       # i2s-version 3
    if not master:
        w32(o+0x90,fake(name+'_txdma')); w32(o+0x94,fake(name+'_rxdma'))
    w32(o+0x98,fake(name+'_irq'))
    if master: w32(o+0xa8,fake(name+'_mclk'))   # function-mclk_frequency NCOf 0
    return o
def run(title,desc_m,desc_s):
    global state; state={}; log.clear()
    slaves=[new_i2s('i2s%d'%i,i,False) for i in range(4)]
    m=new_i2s('master',5,True)
    w32(m+0xc8,4)
    for i,s in enumerate(slaves): w32(m+0xcc+4*i,s)
    p=alloc(32); uc.mem_write(p,desc_m)
    rc=call(CONFIGURE,(m,p)); log.append('-- configure -> %#x; start(master, TX)'%rc)
    rs=call(START,(m,2))
    for s in slaves:
        log.append('-- start(slave, TX)'); call(START,(s,2))
    print('== %s  desc %s'%(title,desc_m.hex(' ')))
    for l in log: print('   '+l)
    print('   final:'); 
    for off,v in sorted(state.items()): print('     %s+%#05x=%08x'%(win(off),off&0xfff,v))
ADT_M=struct.pack('<8I',0x1,0x10002,0,0,0,3,3,0x10100202)
def withrate(d,num,den,bits=None):
    d=bytearray(d); struct.pack_into('<HH',d,0x10,num,den)
    if bits: d[0x1e]=d[0x1f]=bits
    return bytes(d)
run('ADT audio-hdmi-master template, 48 kHz 16-bit',withrate(ADT_M,48,1),None)
run('ADT template, 44.1 kHz 16-bit',withrate(ADT_M,441,10),None)
run('ADT template, 48 kHz 24-bit',withrate(ADT_M,48,1,24),None)
def run_slave(title,d):
    global state; state={}; log.clear()
    s=new_i2s('i2s0',0,False)
    p=alloc(32); uc.mem_write(p,d)
    rc=call(CONFIGURE,(s,p)); log.append('-- configure -> %#x; start(slave, TX)'%rc)
    call(START,(s,2))
    print('== SLAVE %s  desc %s'%(title,d.hex(' ')))
    for l in log: print('   '+l)
ADT_S=struct.pack('<8I',0x0,0x10002,0,0,0,3,3,0x10100202)
run_slave('audio-hdmi-0 template 48k 16b',withrate(ADT_S,48,1))
run_slave('audio-hdmi-0 template 44.1k 16b',withrate(ADT_S,441,10))
run_slave('audio-hdmi-0 template 48k 24b',withrate(ADT_S,48,1,24))
