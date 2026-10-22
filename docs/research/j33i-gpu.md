# J33i GPU (SGX543) -- the P105 driver, ported (2026-10-22, offline)

Status: **built, not yet booted.** Everything below was done on the host from
`work/kc.j33i.macho` (12H847) and `work/j33i-full.dts` (the ADT). Nothing has
touched the box. The driver is P105's `drivers/gpu/drm/apple-sgx`, unchanged.
Background: [p105-gpu.md](p105-gpu.md) (the bring-up notebook),
[../GPU.md](../GPU.md), [j33i-gpu-video.md](j33i-gpu-video.md).

## What differs from P105

| | P105 (iPad mini 1, 12H321) | J33i (Apple TV 3, 12H847) |
|---|---|---|
| ADT node | `sgx`, `gpu,s5l8940x`, `0x5100000 0x18000`, irq 49, brn_31195 | the same |
| SGX gate (no register) | id 92 | id 64 (`power-gates "@"`) |
| its parents | GFX_SYS-CLK, GFX-CLK | id 30 GFX_SYS-CLK (power index 9, `+0x1024`), id 31 GFX-CLK (index 10, `+0x1028`): **the same registers** |
| PMGR after iBoot | `0x300` (off) | `0x300` both (read on the box under Linux) |
| IMGSGX543 kext | Mach-O at `0x80bf1000` | at `0x80ba0000` (`-0x51000`), same size `0x2b000`, same sections |
| microkernel | 12H321 | **byte-identical** (below) |
| cores | 2 (CORE_ID `0x01194201`) | **unknown**: read from hardware at probe |
| framebuffer | 768x1024 | 1920x1080, stride 7680 (simplefb after `j33i_display.c`) |

The device-clocks entries are 44 bytes on J33i: id at `+3`, parents at
`+8..+11`, power-register index at `+12` (`PMGR + 0x1000 + 4 * index`, the same
rule that gives AUDIO `0x109c`, j33i-audio.md section 6), clock-register index
at `+16`, name at `+28`. GFX_SYS-CLK's clock index is 29 (`+0x74`) and
GFX-CLK's is 28 (`+0x70`), the perf-state outputs p105-gpu.md describes.

**The core count is read from the hardware, on iOS as well** (12H847,
`SGXDriver543::start`): `0x80ba0820..0x80ba082a` reads MASTER_CORE_ID
(`+0x4010`), `ubfx #8, #4` -> `this+0x5c8`. The provider property
`sgx-core-count` (string `0x80bada19`) and then the boot-arg `sgx_core_count`
(`0x80ba0868`) can only lower it: the code logs "Invalid SGX core count" and
ignores a value above the hardware's. `this+0x5c0 = this+0x5c4` (`0x80ba08e8`)
and `initSGX` (`0x80ba2918`, P105's `0x80bf3918` - `0x51000`) writes
MASTER_CORE = `this+0x5c0 - 1` at `0x80ba2a1e..0x80ba2a28`. The J33i ADT has
no `sgx-core-count`. `apple-sgx` does the same thing (CORE_ID bits 11:8), so
MP1 or MP2 shows in the first probe line.

## Firmware: identical to 12H321

`__TEXT,__const` (`0x730` bytes, VA `0x80baf8cc`) has the 12H321 hash
(`b59b6d44...`). `__DATA,__data` (`0x16094` bytes, VA `0x80bb1a20`) hashes
differently (`3e3b7aee...`). The only difference is the kext's `kmod_info` at
`__data + 0x15fe8`: `address` `0x80ba0000`, `start` `0x80bacff1` and `stop`
`0x80bad039`. With those three words moved up by `0x51000`, the blob hashes to
12H321's `ee453025...` exactly. The driver never reads those bytes: its last
table (`UK_TAB_BASE + 0x118`, `0x140` bytes) ends at `0x15fe8`, where
`kmod_info` begins. The header's `__data` address comes out as `0x80c02a20`,
the value p105-gpu.md gives for 12H321.

So every offset the driver hard-codes into the blob (UK_CODE/CODE2/BOOT/TAB,
the LIMM patch sites, the handler offsets `0x1fa8`/`0x1bf0`/`0x1e80`/...) holds
unchanged. `scripts/extract-sgx-firmware.py --kernelcache` relocates those three
words and writes the build tag `12H321`, which the driver checks. Both sections
then match the recorded 12H321 hashes, and the header carries 12H321's
addresses. So the file should equal the iPad's, though no iPad copy was on
this host to compare (sha256 `5f929f35...`):

    python3 scripts/extract-sgx-firmware.py --kernelcache ../work/kc.j33i.macho build/firmware-j33i/sgx543.fw

`BOARD=j33iap ./cascadia build` runs that itself when the file is missing
(`KC_J33I=` points elsewhere). It puts the file in the image as
`/lib/firmware/apple/sgx543.fw`. `scripts/build-firmware-j33i.sh` makes it too.

## Testing on the box

The GPU nodes are in `dts/j33iap.dts`, and `CONFIG_DRM_APPLE_SGX=y` is in the
shared `config/p105ap.config`. **`autoboot` is on by default**, so the
microkernel starts about 1.6 s into boot. For the first boot, consider adding
`apple_sgx.autoboot=0` to `chosen/bootargs`. You then see the probe line and
`regs` before the microkernel runs, and start it by hand with `boot`.

1. **Probe**, `dmesg | grep -i sgx`. On P105:

       apple-sgx 35100000.gpu: SGX543MP2 rev 1.2.2, 2 core(s) up, clocks on

   On J33i, expect `SGX543MP1 ... 1 core(s)` or `MP2 ... 2`, with rev 1.2.2
   (brn_31195 predicts it). Also `clk_summary`:
   `grep gfx /sys/kernel/debug/clk/clk_summary` (both enabled, count 1), and
   `peek r 0x3f101024`, `peek r 0x3f101028` -> `0x3ff`.
2. **Registers**: `cat /sys/kernel/debug/apple-sgx/regs`. Master and each core:
   CORE_ID `0x01194n01` (n = cores), MASTER_CLKGATESTATUS2 `0x1f`, core CLKGATECTL
   `0x10055555`.
3. **Microkernel** (automatic with autoboot, or by hand):

       echo 1 > /sys/kernel/debug/apple-sgx/boot
       dmesg | tail
       # apple-sgx 35100000.gpu: 2D engine: framebuffer 1920x1080, 1920 px lines, at GPU 0x90000000
       # apple-sgx 35100000.gpu: starting the microkernel: code at GPU 0x80000000, page directory 0x...
       # apple-sgx 35100000.gpu: microkernel is up: host[0] 0x00000001

   `boot` returns EBUSY while a render node client is open.
4. **Commands** (no GPU work): `echo "hwperf 0" > .../apple-sgx/cmd` and
   `echo "power 2" > .../cmd` (idle), then `echo "power 3" > .../cmd`. Expect
   `taken after ~40 us`.
5. **2D engine** (the first real GPU work). Unbind fbcon first, or it
   repaints over the result:

       echo 0 > /sys/class/vtconsole/vtcon1/bind
       echo "fill 100 100 400 300 0xffff0000" > /sys/kernel/debug/apple-sgx/cmd
       echo "copy 100 100 600 100 400 300" > /sys/kernel/debug/apple-sgx/cmd
       echo "bench 1000" > /sys/kernel/debug/apple-sgx/cmd   # P105: ~112 us per 64x64 fill
       echo 1 > /sys/class/vtconsole/vtcon1/bind

6. **KMS / render node**: `dmesg | grep -E 'render node|screen:|parameter buffer'`,
   and `ls /dev/dri` (card0, renderD128).

**GL (glxgears, kmscube, SuperTux) is not reachable yet.** The Mesa driver and
sgx2d load the sgx2d pack at runtime (`pack.txt`, `sgx_frame.c`
`event_template()`). `./cascadia gpu` (`tools/sgx/mkpack.py`) builds the pack
from `build/firmware/kernelcache.12H321.macho` and `gl-event.pds`, both from the
**iPad's** IPSW, which is not on this host. `tools/iosgpu/rtemu.py` emulates
the kext at P105's addresses (`KLO = 0x80bf1000`). Two routes:

- build the pack from the 12H321 IPSW as on P105 (the kext code is the same);
- or point rtemu at `0x80ba0000` with `kc.j33i.macho`, and cut `gl-event.pds`
  from the 12H847 dyld cache (a different offset; needs its rootfs key).

With a pack in place, the P105 steps apply: `./cascadia mesa`, then
`glxgears` under X, `kmscube`, `es2gears`.

Read too, for the record: PMGR `+0x70`/`+0x74` (`peek r 0x3f100070`,
`0x3f100074`) and the perf state at `+0x300`. `SGX_CLOCK_KHZ` (102600) is
P105's perf-state-2 GFX-CLK and only feeds the microkernel's timer.

## Risks, highest first

1. **Bank-0 reads hang the bus**, as on P105: never `peek r` anything in
   `0x35100000-0x35103fff`. Core banks hang until MASTER_CLKGATECTL gives the
   core a clock. On an MP1, **core 1's bank (`0x3510c000`) does not exist**:
   do not peek it (the driver refuses it once it knows ncores).
2. **MP1 is untested anywhere.** If CORE_ID says 1 core, the driver's sequence
   is iOS's with `cores = 1` (MASTER_CORE 0, MASTER_CLKGATECTL `0x1`, reset
   bits for core 0 only). The microkernel is the one iOS runs on this box, so
   it copes, but Mesa and sgx2d were only ever run on two cores (per-core
   registers in the render path, master `0x510+` per extra core).
3. **Power-on is the first write to the GFX domains on this board.** The
   registers and the `0x300` start value match P105, and the clock gate driver
   is the one already used for audio here. If probe hangs before the probe
   line, boot with `initcall_blacklist=apple_sgx_driver_init` and try it with
   `peek` by hand: `0x3f101024`, then `0x3f101028`, `(v & ~0x10f) | 0xf`, wait
   for `0x3ff`, then `peek r 0x35104010` (master CORE_ID; master bank only).
4. **1080p in the 2D engine**: stride field 959, 60 x 34 regions, 8 MB of
   framebuffer in the 16 MiB window. All inside the encodings, but P105 only
   ever ran 768x1024.
5. **GPU clock**: if J33i's GFX-CLK is not 102.6 MHz, only the microkernel's
   timer period is off (lockup detection). It is not a hang.
6. HPERF-NRT (`+0x102c`, `0x300` here) is left off, as on P105, where it was
   tested off.
