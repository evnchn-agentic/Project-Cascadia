# J33i: GPU and video decode feasibility (Apple TV 3, S5L8947X)

Read-only, offline desk study. Nothing touched the device. Sources: this
tree (`docs/GPU.md`, `docs/research/p105-gpu.md`, `p105-mesa.md`, `mesa/`,
git log), `work/j33i-full.dts`, `work/kc.j33i.macho`, and background
knowledge of public projects.

Labels: **READ** = seen in this tree/ADT/kernelcache; **KNOW** = public fact
I am confident of; **EST** = my estimate; **GUESS** = inference, unchecked.

## 1. What the project already has for SGX543

READ. This is far beyond "firmware only". The iPad mini 1 (P105) port has:

- A kernel DRM driver `apple-sgx` (render node `/dev/dri/renderD128`, KMS on
  `card0`) that boots **Apple's own SGX microkernel** (`sgx543.fw`, taken
  from the IPSW by `scripts/extract-sgx-firmware.py`) with iOS's register
  sequence, and submits TA + 3D commands through iOS's own CCB format.
- A **Gallium Mesa 26.1.8 driver** (`mesa/`, milestones M12..M42) with its own
  USSE assembler/compiler. State (per `mesa/README.md`, GPU.md): GLES 1/2,
  desktop GL 2.1 via GLX and EGL, dEQP-GLES2 16 859 pass / 91 fail, piglit
  run, X with modesetting + glamor, weston, xfwm4 composition, rotation,
  60 fps vblank. glxgears 60 fps; SuperTux 0.7 SDL renderer 33-43 fps.
  (llvmpipe on the same box was 0.22 fps for the same game, so the GPU is
  the only usable path for any GL work.)
- Needs from the IPSW (not redistributable): the microkernel, one 168-byte
  PDS template from iOS's GL driver in the dyld cache, and the kernelcache
  (kext code is emulated at pack-build time to compute render-target data).
- Caveats seen in the notes: GPU clock on the iPad is ~102.6 MHz (perf state
  2) with 200 MHz known to be the nominal PLL; no clear evidence in the notes
  that the clock was raised. Render-target/texture limits and the
  EGL/vsync/fullscreen path ("shows nothing") are open.

**J33i delta (READ/GUESS).** The ADT node is `gpu,s5l8940x`, `reg = <0x5100000
0x18000 0xf10b000 0x1000>`, irq 49, clock-gate 64, `opengl-standard = 512`,
`brn_31195`. The kernelcache has `com.apple.driver.IMGSGX543` (CFBundleVersion
115.5, executable 0x2b000 bytes, READ) - the same kext family the iPad ships.
The iPad is an **MP2**; the Apple TV 3 (S5L8947X) is a single-core A5 so it
should be **MP1** (GUESS, not verified: the task brief says MP1; the ADT does
not state a core count). Work this implies: the driver reads core count from
MASTER_CORE and already treats it as a variable ("cores - 1"), but the
microkernel firmware, register-bank count, tile/TA setup and the emulated
kext render-target code were all exercised on MP2 only. Expect bring-up
bugs, not a rewrite. The j33i IPSW must supply its own microkernel (may be
byte-identical or MP1-specific; unknown). Power gating/clock tree (clock-ids
295, gate 64) differs from P105 and needs the same pmgr RE the iPad took
(p105-gpu.md was ~5 days of work for that part).

## 2. SGX5xx on Linux: other options

- **TI/IMG DDK (KNOW):** TI ships `ti-sgx-ddk-km` (pvrsrvkm/omaplfb/bufferclass)
  plus the closed `ti-sgx-ddk-um` EGL/GLES blobs for OMAP3/4/5, AM335x
  (SGX530/540/544). They are tied to specific kernel versions (mostly 3.x-5.x
  trees, patched forward by community for newer kernels), to a given DDK
  version, and use the **IMG SGX microkernel built into the km driver**, not
  Apple's. Nothing here targets SGX543 on Apple glue; the km would need the
  543 core/BRN config for this exact rev (1.2.x), Apple's bus/MMU-window
  wiring (the project found GPU memory at 0x80000000, BIF tiling windows) and
  the Apple clock/power. Not plausible to drive Apple's MP1 without
  reproducing what `apple-sgx` already is. Also 32-bit ARM glibc blobs under
  a musl userland. **Dead end.**
- **Mainline (KNOW):** `drivers/gpu/drm/imagination` + Mesa `powervr` are
  Series-AXE/Rogue onward, a different ISA (not USSE). `gma500` (SGX535 on
  Poulsbo/Cedarview) is KMS/2D only. Mesa has no upstream USSE/SGX
  compiler. Maemo Leste and others keep SGX530 working with the blob. I know of
  **no open-source SGX5xx 3D driver other than this project's**.
- **Apple-specific (READ):** Apple's microkernel + CCB protocol and its USSE
  programs are what the project RE'd; that is the only viable route and is
  done for MP2.

Conclusion: the project's own Mesa driver is the only plausible GPU path for
the Apple TV 3. Retargeting MP2 -> MP1 is the work, not a new driver.

## 3. VXD (video decoder)

ADT (READ): `vxd@8100000`, compatible `"vxd,s5l8940x","vxd,s5l8920x"`,
`reg 0x8100000 size 0x100000`, irq 50, clock-gates 94 95, clock-ids 297,
`vxd-version = 2`, `h264-playback-level = ")"` (0x29 = **H.264 Level 4.1**),
`allow-oold`, `minimize-power-gating`, power-gates "^". Kernelcache (READ)
has `com.apple.driver.AppleVXD390` (kext named VXD390).

IP identity (KNOW for lineage, GUESS for exact rev): Imagination
**VXD3xx/VXD39x-class** (successor of MSVDX; the "D5500/D5520" naming
belongs to later VXD38x-39x boards). A5-era Apple parts license the
VXD390 (up to H.264 High L4.1/1080p30-ish). Capabilities: H.264, MPEG-4,
likely VC-1/MPEG-2; no VP9/AV1/HEVC.

Linux lineage (medium confidence on all):
- Upstream-ish: TI's **img-vxd / "imgvdec"** V4L2 stateless driver for the
  Imagination D5520 (AM62A / J721S2) was posted to linux-media and sits
  under `drivers/staging/media/` (`vxd`). It targets the newer D5xxx firmware
  interface (MTX firmware + message queue), not the A5-era VXD390 register
  interface. Not directly applicable.
- Intel Poulsbo/Medfield **MSVDX** (gma500/psb) is the same ancestor with an
  open-ish userspace (`libva-driver-psb`/psb-video); only very distant ancestry.
- No public Linux driver for Apple's VXD390; I know of none.

RE needed (EST): large. Apple's kext (`AppleVXD390`) is the driver; the
**decoder runs firmware on an embedded MTX/MSVDX microcontroller or is
register-driven** depending on rev (GUESS, check). Work items: (a) power/clock
(gates 94/95, id 297, plus pmgr RE, same difficulty as SGX), (b) kext
disassembly to get MMIO map, command/message format, IRQ semantics (the
kernelcache is in hand, tools/audio/j33i_kc.py exists), (c) DMA/MMU setup
(shares the BIF-style MMU idea with SGX? GUESS), (d) per-slice stream
programming plus reference/DPB buffer formats (Apple's tiled output format
needs untiling or a display/GPU path), (e) a V4L2 stateless or custom
userspace player, (f) validation with the iOS tooling (gltrace-like capture
via a jailbroken device is not available here per the brief). Without a
device-side trace channel this is **weeks to months** and uncertain: the
SGX route benefited from iOS device captures. EST 6-12 weeks if a trace
channel exists, much more without, and it only helps H.264 <=1080p30 High 4.1
files; YouTube's modern streams (VP9/AV1) would not benefit.

## 4. Software decode on one A9 @ ~850 MHz

EST (no measurement; no benchmarks in this tree). ffmpeg/libavcodec has
hand-written NEON for H.264 IDCT, MC (qpel), loop filter, intra prediction;
A9 is out-of-order, ~1.4-1.6x the per-clock throughput of an A8.

| Content (H.264) | Realistic on 1 core @ 850 MHz |
|---|---|
| 480p30, 1-2 Mbps | smooth (~25-40% CPU) |
| 720p30, <= 2.5 Mbps, High, no B-pyramid abuse | marginal-to-OK; 90-100% CPU, may drop frames |
| 720p24 cinema/Blu-ray-ish | OK if <= ~4 Mbps; lower with `-skip_loop_filter nonref` |
| 1080p30, 4-8 Mbps | **no**: ~8-14 fps; with `skip_loop_filter all` + `lowres` tricks maybe 18-20 fps, visibly broken |
| 1080p24 low-bitrate | borderline-unwatchable |

Colour conversion adds cost if done on CPU (swscale 1080p ~10+ ms/frame on this
class): do YUV->RGB + scaling on the GPU. Also: no frame threading gain with 1
core; the second Cortex-A9 core (if the dts's `#main-cpus = <2>` is real) is
fused off/unavailable (`cpu-max-core-count = 1`, READ). Memory is 512 MB; also
audio (HDMI/I2S) decode is negligible.

Practical "watch video on the box" path today:
1. Software decode with `mpv`/ffmpeg, `--vd-lavc-skiploopfilter=nonref`,
   `--vd-lavc-fast`, 720p or 480p H.264 files; `yt-dlp -f 'bv*[vcodec^=avc1][height<=720]+ba[acodec^=mp4a]'`.
   (YouTube's VP9/AV1 and 1080p VP9 are out of reach; avc1 480p/720p exist.)
2. Output: mpv `--vo=gpu` with Mesa SGX driver (GLES2 YUV shader; upload 3
   planes, ~2 MB/frame at 720p) once the MP1 GPU works; until then `--vo=fbdev`/drm
   with CPU conversion (480p only).
3. Netflix/Disney: **dead end** (Widevine L1/PlayReady need a TEE/browser/DRM
   stack; L3 in Firefox/Chromium on a 850 MHz single core is not viable anyway).
   A browser for YouTube proper is also impractical; use yt-dlp + mpv.

## 5. Ranked recommendation

| # | Item | Effort (EST) | Worth |
|---|---|---|---|
| 1 | Software decode path: ship mpv/ffmpeg+yt-dlp, 480p/720p avc1, document limits | 1-2 days | high, available now, no hardware work |
| 2 | Bring up `apple-sgx` on j33i as MP1 (power/clock ADT RE, MP1 firmware, regs; re-run existing Mesa) | 1-3 weeks if the MP2 driver mostly works, more if pmgr/clock RE is hard | high: gives GL, GLES YUV video output, UI, ~30% CPU relief for conversion/scaling |
| 3 | GL video output (mpv `vo=gpu` with SGX Mesa; YUV textures) after #2 | 2-5 days after #2 | medium-high |
| 4 | VXD390 H.264 decode (RE of AppleVXD390, V4L2 shim) | 2-3+ months, high risk; only H.264 <=1080p30 | medium: the only way to real 1080p, but only for H.264 sources, and display of tiled output |
| 5 | TI/IMG DDK pvrsrvkm on Apple MP1 | no | **dead end** (wrong fw, rev and glue) |
| 6 | Netflix/Widevine on the box | no | **dead end** |
| 7 | 1080p software decode | no | **dead end** on one A9 @ 850 MHz |

Recommended order: 1, 2, 3 (cheap wins and prerequisite for a usable UI),
then decide on 4 only if 1080p H.264 is a goal; start 4 with a static RE
pass over `AppleVXD390` (kext offset in kc.j33i.macho around 0x80a?????, size
not read exactly; `IMGSGX543` is 0x2b000 bytes, READ) to bound the effort
before committing.

## Unverified items worth checking first

- Whether j33i's GPU is truly MP1 (read MASTER_CORE on the device) and whether
  its IPSW microkernel matches the iPad's.
- Whether the iPad's 102.6 MHz GPU clock is a limit for 720p YUV compositing
  (it is ample: 1280x720x30 fps is a trivial fill).
- The exact VXD390 interface (MTX firmware or fixed register file).

## Measured: software H.264 decode on the box (2026-10-22 15:36)

`ffmpeg -benchmark -threads 1 -i clip -f null -` (Alpine 3.24 ffmpeg, NEON), 10 s testsrc2 clips, x264 High,
yuv420p, 30 fps, made on the Mac:

| Resolution | Bitrate | Decode | Real time |
|---|---|---|---|
| 854x480 | 1.2 Mbit/s | 60 fps | 2.01x |
| 1280x720 | 2.5 Mbit/s | 29 fps | 0.97x |
| 1920x1080 | 5 Mbit/s | 14 fps | 0.45x |

This is decode only (no colour conversion or display). 480p has headroom for conversion and audio; 720p30 is at
the limit, so it needs the YUV-to-RGB step on the GPU (mpv `--vo=gpu` on the SGX Mesa) and probably
`-skip_loop_filter` to play smoothly; 1080p needs the VXD390.
