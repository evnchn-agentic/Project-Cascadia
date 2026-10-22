# J33i GPU (SGX543) -- the P105 driver, ported (2026-10-22, offline)

Status: **kernel side up on the box** (e45ef6f: "SGX543MP1 rev 1.2.2, 1
core(s) up, clocks on", DRM on the 1920x1080 screen, "microkernel is up" 74 ms
after the kick, 2D fill/copy right on the HDMI capture). **The userspace side
(the pack, sgx2d, GL) is built but not booted.** The port was done on the host
from `work/kc.j33i.macho` (12H847) and `work/j33i-full.dts` (the ADT). The driver is P105's `drivers/gpu/drm/apple-sgx`, unchanged.
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
| cores | 2 (CORE_ID `0x01194201`) | **1** (an MP1), read from hardware at probe |
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
MP1 or MP2 shows in the first probe line. On this box it is MP1 (e45ef6f).

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

   On J33i (seen 2026-10-22): `SGX543MP1 rev 1.2.2, 1 core(s) up, clocks on`. Also `clk_summary`:
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

Read too, for the record: PMGR `+0x70`/`+0x74` (`peek r 0x3f100070`,
`0x3f100074`) and the perf state at `+0x300`. `SGX_CLOCK_KHZ` (102600) is
P105's perf-state-2 GFX-CLK and only feeds the microkernel's timer.

## The userspace side: the sgx2d pack, and GL (2026-10-22, offline)

Status: **built, not booted.** Mesa and sgx2d read a "pack"
(`/usr/local/lib/sgx2d/pack.txt` and its images). Mesa, on a kernel with UAPI 3
(this one), builds its frame itself and takes only one thing from the pack: the
3D pass's event program, at the `pds` address (`sgx_frame.c`
`event_template()`). sgx2d takes all of it. The pack is now made for J33i from
the Apple TV's own files, and the image carries it.

**The event program is the iPad's, byte for byte.** It is a 0xa8-byte template
in `IMGSGX543GLDriver`'s `__TEXT,__const`, in the dyld shared cache. In 12H847's
`dyld_shared_cache_armv7` (347,752,882 bytes, rootfs `048-37456-047.dmg`) the
GL driver is at `0x20199000`, its `__const` at `0x202a78b0` (file `0x10e8b0`,
`0x66a0` bytes). Hashing every 0xa8-byte window of that section finds exactly
one window with the 12H321 hash `a1b855d4...2285` (`pds.py` `EVENT_SHA256`). It is
at section `+0x2328`, VA `0x202a9bd8`, file offset `0x2a9bd8`. In the iPad's
cache the same template is at file offset `0x46ebd8`, so only its position
moved. `ONLY=gpu scripts/build-firmware-j33i.sh` cuts it from the IPSW
(`rootfs-extract.py ...dyld_shared_cache_armv7@2a9bd8+a8`, key from
theapplewiki's 12H847 page) and checks the hash.

**The kext's render-target code runs at J33i's address.** `tools/iosgpu/rtemu.py`
now finds the kext's base in the kernelcache. It uses the same scan as
`extract-sgx-firmware.py`: a Mach-O on a page boundary in `__PRELINK_TEXT` whose
`__cstring` holds `SGXDriver543`. That gives `0x80ba0000` here and `0x80bf1000`
on 12H321, and `SGX_KEXT_BASE=` overrides it. Every address it uses is now
relative to that base (RT init `+0x6aac`, 3D block `+0x4eec`, stubs `+0xd3f4`,
`+0xd4f4`, `+0xd8b4`, `+0xd924`). Two checks:

- With `kc.j33i.macho`, `mesa/host/rt-test.py` compares rtemu with Mesa's own
  `sgx_rt.c`, which was checked against the iPad's kext. 61 of 61 sizes match,
  and so do 1920x1080, 1280x720 and 1080x1920.
- With the old base (`SGX_KEXT_BASE=0x80bf1000`) on the same kernelcache,
  rtemu fails, so the check can fail.

With no `BOARD` set, the generators' output is unchanged: a 768x1024 pack made
before and after the screen-size change is byte-identical.

**What the build makes.** Run `BOARD=j33iap ./cascadia gpu` to make it by hand.
`BOARD=j33iap ./cascadia build` also makes it the first time it is missing,
from `KC_J33I` (default `../work/kc.j33i.macho`) and the 12H847 IPSW (default
`../ipsw/`). It goes to `build/sgx2d-j33i` and then into the image:

| image path | what |
|---|---|
| `/usr/local/lib/sgx2d/` | the pack for the box's **MP1**, at **1920x1080** (`screen 1920 1080`): `pack.txt`, `tmpl.bin`, `m_*.bin` |
| `/usr/local/lib/sgx2d-mp2/` | the same for an **MP2** (in case another unit has two cores): `pack.txt`, `m_87b00000.bin`, `m_87c00000.bin` (the per-core parts of the render target), the rest links to the MP1 pack's |
| `/usr/local/lib/sgx2d/{sgxinfo,sprites,demo2}` | one static ARM binary (`sgx2d-tools`), named three ways |

The core count only matters to sgx2d. Mesa reads it from the kernel. The
screen size comes from `SGX_SCREEN` (default 768x1024, P105 unchanged).
`BOARD=j33iap` sets 1920x1080. For a box booted with `atv_hdmi=720`, rebuild
with `SGX_SCREEN=1280x720 BOARD=j33iap ./cascadia gpu`.

**Mesa is not in the image, and it cannot be built offline.** `./cascadia mesa`
clones Mesa 26.1.8 (`mesa/fetch.sh`) and builds it in an Alpine armv7 image
under emulation. Neither the source nor that image is on this host. Its
libraries also need X and Wayland libraries (libX11, libxcb, libdrm,
wayland-libs, ...), and the RAM root has none of them. The image budget is
another limit. The bundle is now 29,129,410 bytes, and `/linux.kc` is the
bundle plus 12.5 % (literal-only LZSS), about 32.8 MB, while iBoot loads a
file of about 32 MB. libgallium and those libraries would add several MB
compressed. So Mesa goes in **after boot, into the RAM root** (512 MB),
from a host that is online.

## Running it on the box

**Boot this build.** `./cascadia build` writes `output/staging-bundle.bin`.
The box boots `/linux.kc`, which `tools/atv-fsboot.sh install` makes from that
bundle (`tools/mk-fake-kc.py`) and writes to the system partition. That step
needs evnchn's yes. An older `/linux.kc` has no `/usr/local/lib/sgx2d`.
`build/firmware-j33i/linux.kc` from 13:18 is one of those.

Then do this once the microkernel is up (section above, step 3). Each block
assumes the one before it worked. `BOX` is the box's address: `10.55.0.2`
over the USB link, or its LAN address after an fsboot (`192.168.50.138` on
2026-10-22).

**1. The render node.** Run it over ssh or on the HDMI console:

    /usr/local/lib/sgx2d/sgxinfo

Expect `/dev/dri/renderD128:`, `uapi version 3`, `cores 1`,
`microkernel up 1`, `framebuffer width 1920`, `height 1080`, `stride 7680`,
`framebuffer va 0x90000000`, three `buffers:` lines that pass, then `all good`.
**Note `cores`**: it picks the pack below (1 on this box). If the width is not 1920, do not
run the sgx2d demos. Their pack draws a 1920x1080 frame into the framebuffer.

**2. sgx2d, the first GPU drawing without Mesa.** fbcon must let go of the screen,
or it repaints over the output:

    P=/usr/local/lib/sgx2d          # cores 1 (this box)
    # P=/usr/local/lib/sgx2d-mp2    # cores 2
    echo 0 > /sys/class/vtconsole/vtcon1/bind
    /usr/local/lib/sgx2d/sprites $P 200 600     # bouncing sprites, ~10 s
    /usr/local/lib/sgx2d/demo2 $P               # fills, blend modes, a streaming texture
    echo 1 > /sys/class/vtconsole/vtcon1/bind

`sprites` should print `600 frames, 200 sprites: N fps` (P105, 768x1024 MP2: ~217
fps). Expect well under that here: 1080p has about 2.6 times the pixels and
this is one core, not two. On HDMI you should see sprites over the whole 1920x1080 screen.
`demo2`'s bottom band is 768 pixels wide (its fill is hard-coded), which is
expected. If it fails:

- `sgx2d_open: -N`: wrong pack directory, or no render node.
- `frame N: -N`: a render timed out. Check `dmesg | tail` and
  `renders timed out` in `sgxinfo`.
- A picture sheared in diagonal bands: the pack's screen does not match the
  framebuffer (`atv_hdmi=720`; rebuild with `SGX_SCREEN`).

**3. GL (Mesa).** On the host, online, with the box up on the USB link (or
`./cascadia net on` and its Wi-Fi):

    ./cascadia mesa --install root@$BOX     # build (an hour or more the first time), then over ssh

`--install` unpacks into `/`. Only `--auto` refuses a RAM root. Here `/` is
the RAM root, so it is gone at the next boot. Then, on the box, with the internet shared
(`./cascadia net on` on the host):

    apk update && apk add libdrm libx11 libxcb libxext libxfixes libxshmfence \
        libxxf86vm libxrandr wayland-libs-client wayland-libs-server kmscube mesa-demos
    # (Alpine 3.24 names; `apk search` if one has moved.  mesa-demos and kmscube
    # pull in Alpine's own Mesa too; the install's ld path keeps ours first.)
    echo 0 > /sys/class/vtconsole/vtcon1/bind
    sgx-gl glclear 100 --fb                 # clears on the GPU, every pixel checked
    SGX_DEBUG=fps sgx-gl kmscube            # KMS + GBM on /dev/dri/card0
    sgx-gl eglinfo                          # mesa-demos
    echo 1 > /sys/class/vtconsole/vtcon1/bind

`/etc/ld-musl-armhf.path` (from the install) already puts our Mesa first.
`sgx-gl` also sets `LD_LIBRARY_PATH`, `GBM_BACKENDS_PATH` and `PATH` to its prefix. Expect:

- `glclear`: every pixel right.
- `kmscube`: a spinning cube on HDMI. P105 measured 194 fps at 768x1024.
- `eglinfo`: the renderer string `PowerVR SGX543MP2`. It is hard-coded in
  `sgx_screen.c`, so an MP1 shows it too.

A missing `/usr/local/lib/sgx2d` would show as `sgx: no 3D event program in
/usr/local/lib/sgx2d`. The image has it, so that message means it is gone.
`glxgears` needs an X server (`desktop`, M36), which this root does not have.


## Risks, highest first

1. **Bank-0 reads hang the bus**, as on P105: never `peek r` anything in
   `0x35100000-0x35103fff`. Core banks hang until MASTER_CLKGATECTL gives the
   core a clock. On an MP1, **core 1's bank (`0x3510c000`) does not exist**:
   do not peek it (the driver refuses it once it knows ncores).
2. **This box is an MP1, and no 3D render has run on an MP1 anywhere.**
   The kernel side works with 1 core (e45ef6f: microkernel up, 2D engine
   right). Mesa and sgx2d were only ever run on two cores, with per-core
   registers in the render path and master `0x510+` per extra core. The
   first `sprites` or `glclear` is the first MP1 3D render.
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

Userspace (the pack, sgx2d, Mesa), highest first:

7. **The image size.** The pack and tools add 282,624 bytes to the bundle
   (28,846,786 -> 29,129,410), which makes `/linux.kc` about 32.8 MB
   (bundle x 1.125 + about 5 KB). The largest size iBoot is known to have
   loaded is 32,457,820 bytes. If fsboot stops loading, the size is the
   suspect. To build without it, use `SGX_PACK_IMAGE=0 BOARD=j33iap
   ./cascadia build`. Then copy `build/sgx2d-j33i/{pack,bin}` to
   `/usr/local/lib/sgx2d` over ssh after boot.
8. **The sgx2d pack at 1080p has never drawn.** rtemu's render-target data
   matches Mesa's `sgx_rt.c` at 1920x1080, but the rest of sgx2d's frame at
   that size has not been run: the parameter buffer is still the 8.5 MB sized
   for 768x1024 (a partial render if it runs out), and the 1080p
   framebuffer-load and emit words. The pack in `/usr/local/lib/sgx2d` is
   the MP1 one, and its render target is laid out for one core. The wrong one
   (`sgx2d-mp2` on this box) should give a timeout, not a picture.
9. **The rtemu offsets are 12H321's minus 0x51000.** They are checked by
   disassembly (prologues at `+0x6aac` and `+0x4eec`, PIC stubs at the four
   stub sites) and by the 64-size `rt-test.py` match. That covers the
   render-target code, not every path in the kext.
10. **Mesa needs the network**, at build time (on the host) and for its
    runtime libraries (`apk add` on the box). It lives in the RAM root and is
    gone at reboot. Mesa's renderer string says MP2 on this MP1. Mesa takes
    the core count for its render targets from the kernel
    (`APPLE_SGX_PARAM_NUM_CORES`), not from the pack.
11. sgx2d does not compare the pack's screen with the kernel's framebuffer. A
    1080p pack on a 720p framebuffer writes past the end of it. Only `sgxinfo`
    guards against that.
