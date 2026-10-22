# J33i display: switching the HDMI output from iBoot's 720p to 1080p

Static reverse engineering, read-only and offline, of what Linux must program
to move the Apple TV 3 (AppleTV3,2, J33iAP, S5L8947X, iOS 8.4.4 / 12H847)
from iBoot's 1280x720p60 to 1920x1080p60. Nothing touched the device.

Sources:

- iBoot `build/firmware-j33i/iBoot.j33i.bin` (load base 0x9ff00000,
  Thumb-2), disassembled with r2 and `arm-none-eabi-objdump`. It differs
  from `iBoot.fsboot` in 172 bytes. None of them is in the display or HDMI
  code (0x9ff01000..0x9ff03000, 0x9ff13000..0x9ff16200) or in the
  timing/PHY tables (0x9ff47000..0x9ff4a000) (READ, `cmp -l`).
- The kernelcache `work/kc.j33i.macho`: kexts `AppleSamsungHDMITX`
  (0x80a34000), `AppleH4PDisplayPipe` (0x80c0d000), `AppleH4PRGBOUT`
  (0x80c24000), disassembled with `tools/audio/j33i_kc.py`.
- The ADT `work/j33i-full.dts`, and the live iOS IORegistry dumps
  `work/ioreg-service.txt` / `work/ioreg-devicetree.txt`. The HDMITX node in
  `ioreg-service.txt` carries a **register snapshot taken while iOS was
  driving 1080p** (`RegisterSets`, line 3713) and an **EventLog with the exact
  link timing** iOS started.
- Mainline `build/linux/drivers/gpu/drm/exynos/{exynos_hdmi.c,regs-hdmi.h}`.

Tags:

- **READ @addr**: seen in disassembly, a table, or a dump.
- **READ(live)**: in the iOS register snapshot or EventLog.
- **GUESS**: inferred, not seen.

iBoot addresses are 0x9ffxxxxx. Kext addresses are 0x80xxxxxx (12H847).
Physical register addresses are absolute.

## 0. The answer in six lines

1. The scanout path is **framebuffer → DisplayPipe (0x3a200000) → RGBOUT
   (0x39600000) → HDMI TX (0x39700000) → HDMI PHY (I2C bridge
   0x39780000)**. The `scaler0/1` nodes are the M2 memory-to-memory scaler.
   They are not in the scanout path. (READ)
2. **No PMGR clock changes per mode.** The display blocks run on a fixed
   205.2 MHz clock. The pixel clock is made by the HDMI PHY PLL. iBoot's
   display path never writes a PMGR divider, only clock gates.
   (READ, see section 1.3)
3. HDMI core timing and TG registers for 1080p are **READ(live)** from the
   iOS snapshot. They match iBoot's own register formulas fed with CEA 1080p
   timing, value for value (section 3.2).
4. **PHY config for 148.5 MHz** is READ twice and the two copies agree: iBoot's
   built-in table (row 87 at 0x9ff49610) and the ADT `phy-cfg` property
   (row 86), which the iOS kext loads (READ @0x80a35516). Against the 74.25 MHz
   row now running, **only 3 bytes differ**: PHY regs 3, 16 and 27. Do not use
   mainline's `hdmiphy_v14` 148.5 row: its PLL bytes differ from this silicon's.
5. The display pipe needs four sizes changed, one stride, and the
   `0x3960006c` line offset. A 1080p 32 bpp buffer (0x7e9000 bytes) fits in
   the existing reserved `/vram` region at 0x9f570000.
6. Smallest experiment: about 40 `memrw` writes from Linux userspace.
   Verify with the ES02 snapshot JPEG: it should come out 1920x1080 instead
   of 1280x720. Recovery is a reboot.

## 1. Pipeline blocks and register windows

### 1.1 Blocks

| Block | Phys | ADT node / kext | Role | Source |
|---|---|---|---|---|
| DisplayPipe ("ADFE") | 0x3a200000 (0x7000) | `rgbout` reg[0]; `AppleH4PDisplayPipe` (instance `AppleH4DisplayPipe`) | Planes (fb address, stride, format, sizes, scale), blender output size | READ: iBoot 0x9ff014d4/0x9ff015a4; kext 0x80c14390 writes 0x1030 |
| RGBOUT | 0x39600000 (0x1000) | `rgbout` reg[1]; `AppleRGBOUT` | Output stage: size, line offset, gamma LUT, RGB/YCbCr select, `RGB_INT_STATUS` | READ: iBoot 0x9ff16054/0x9ff160b8/0x9ff160e0; kext 0x80c25924 |
| HDMI TX | 0x39700000 + 9 windows | `hdmi`; `AppleSamsungHDMITXControllerS5L8947X` | Exynos v1.4 HDMI IP: CTRL, CORE (timing, infoframes), TG | READ (see `j33i-audio.md` section 2) |
| HDMI PHY via I2C bridge | 0x39780000 (module 8) | `hdmi` reg[8]; `writeI2CBridgeReg`, `writePHYConfigReg` | 32 PHY config bytes. The PHY sits at I2C address 0x38. | READ (section 1.2) |
| M2 scaler 0/1 | 0x38300000, 0x38400000 | `scaler0/1`; `AppleM2ScalerCSC` | Memory-to-memory scale and CSC for the renderer. **Not scanout.** | READ (kext class), GUESS (role) |
| `dwi` | 0x3f700000 | `AppleS5L8940XDWI` | PMU voltage wire. Not display, despite the name. | READ |

There is no `clcd` node on J33i.

HDMI TX modules: the iBoot offset table at 0x9ff473a4 matches the ADT `reg`
order:

| Module | Phys | Contents |
|---|---|---|
| 0 | 0x39700000 | CTRL |
| 1 | 0x39710000 | CORE |
| 2 | 0x39730000 | SPDIF |
| 3 | 0x39740000 | I2S |
| 4 | 0x39750000 | TG |
| 5 | 0x39760000 | EFUSE |
| 6 | 0x39770000 | CEC |
| 7 | 0x39717000 | HDCP |
| 8 | 0x39780000 | PHY bridge |

Every CORE and TG register holds one byte in a 32-bit slot. A 16-bit value
is written low byte at `reg`, high byte at `reg+4`. This is the Exynos
layout. iBoot's writer is 0x9ff1536c: byte stores, 4-byte stride (READ).

### 1.2 The PHY bridge (module 8)

- `hdmi_controller_start` (0x9ff1466c) initialises the bridge:
  - `+0x2c = 1`;
  - `+0x80 = 0x70`, polled until it reads back 0x70. This is the 8-bit form
    of the ADT `hdmi-iic-cfg` byte 0x38, which is the Exynos 4212 HDMI PHY
    I2C address.
  - `+0x20 = 0x3f00`, `+0x00 = 0x281`, `+0x18 = 0`, `+0x30 = 0`,
    `+0x34 = 0xffffffff`, `+0x04 = 0xd0`.

  (READ)
- PHY register `n` (n = 0..31) is byte lane `n & 3` of the word at
  **0x39780088 + (n & ~3)**. The words run 0x39780088..0x397800a4 (READ:
  iBoot `write_phy_config_reg` 0x9ff15830 and the word table at 0x9ff473c8).
- **Busy** is `0x39780084` bit 0. iBoot polls it before and after every
  write, up to 20 tries with a 1 µs sleep each (0x9ff15538). iOS refuses to
  write while it is set (READ @0x80a3e17c).
- iBoot writes **one byte at a time** (32-bit read-modify-write of the
  word). iOS writes **whole 32-bit words** (0x80a3e168, called 8 times from
  `configurePHY` 0x80a3e1dc). Both work (READ).
- PHY register numbering matches mainline: 0x1f = MODE_SET_DONE
  (`HDMIPHY_MODE_SET_DONE` 0x7c >> 2) and 0x1d = power (`HDMIPHY_POWER`
  0x74 >> 2) (READ in iBoot; the mainline equivalence is a GUESS).

The PHY register image is **not** the mainline `hdmiphy_v14_configs[]` row.
Apple's PHY rows (iBoot table at 0x9ff47414 and ADT `phy-cfg`) are 100 bytes
each:

- `u32 pixel_clock`;
- three 32-byte configs, for 8, 10 and 12 bpc. Byte 31 of each config is a
  "not available" flag; only 8 bpc is populated.

The finder (0x9ff152d4) picks the nearest row within ±0.5 % and returns
`row + 4 + 32*depthcode` (READ). The kext's finder (0x80a39d9c) is the same
code over `phy-cfg` (READ).

### 1.3 Clocks

- `clock-ids` of both `rgbout` and `hdmi` = 294 = `clock-frequencies[0x26]`
  = **205 200 000 Hz** in the live ADT. `rgbout minimum-frequency` =
  102.6 MHz (READ(live)). 205.2 MHz is above 148.5 MHz, and iOS runs
  1080p60 on this hardware (READ(live) EventLog). So the fixed pipe clock is
  sufficient. That the pipe pulls pixels against the TG through a FIFO is a
  GUESS.
- iBoot's display path calls only `clock_gate(id, on)`
  (0x9ff20408: `PMGR 0x3f101008 + 4*id`). The gates are:

  | Gate id | PMGR register | Called from |
  |---|---|---|
  | 0x0f | 0x3f101044 | 0x9ff1f43c |
  | 0x15 | 0x3f10105c | 0x9ff1f43c |
  | 0x14 | 0x3f101058 | 0x9ff16024 |
  | 0x19 | 0x3f10106c | 0x9ff16024 |
  | 0x50 | 0x3f101148 (HDMI) | 0x9ff1466c |

- Nothing in `configure_video` (0x9ff14bb0..0x9ff15228), its helpers
  (0x9ff15360..0x9ff15830), `hdmi_controller_start` or the display init
  (0x9ff028bc and callees) loads any other 0x3f1xxxxx/0x3f2xxxxx constant
  (READ, a negative search over those functions).
- The `display-timing` row's `host_clock_id` field (= 11) has no reader in
  those functions (READ). The only readers of the timing pointer are
  0x9ff028bc and 0x9ff02c68.
- So **the pixel clock is the PHY PLL, set by the PHY config bytes**.

## 2. iBoot's 720p bring-up, step by step (READ)

Call chain:

- `fcn.9ff1f43c`: clock gates 0x0f and 0x15, then display init
  `fcn.9ff028bc`.
- The timing comes from the `display-timing` env/ADT name, default
  `"720p-hdmi"`. The table is at 0x9ff47088.

The timing row, in iBoot's own field order (READ, 0x9ff47088):

```
name "720p-hdmi", host_clock_id 11, pixel_clock 74250000, dot_pitch 243,
h: active 1280, [0x14]=110, [0x18]=220, [0x1c]=40 (sync)
v: active  720, [0x24]=5,   [0x28]=20,  [0x2c]=5  (sync)
neg flags 0, depth 24, display_type 7 (HDMI)
```

> **Porch trap (READ).** `fcn.9ff13670` copies `[0x14]` into the HDMI
> config's *back_porch* slot and `[0x18]` into its *front_porch* slot (the
> same for vertical). The HDMI code computes `H_SYNC_START = front_porch - 2`.
> The iOS 1080p snapshot confirms that slot is the CEA front porch:
> `H_SYNC_START = 86 = 88 - 2`.
>
> So **iBoot's 720p physically uses hfp=220/hbp=110 and vfp=20/vbp=5**, the
> CEA porches swapped. Sinks lock on DE, so it works.
>
> For 1080p, use **CEA values in CEA slots** (iOS does). Do not mirror
> iBoot's swap.

Steps, in order:

| # | What | Writes (720p values) | Where |
|---|---|---|---|
| 1 | Gates on | gate 0x14, 0x19 → 0x3f101058, 0x3f10106c | 0x9ff16024 |
| 2 | Store HDMI config | pixel clock, totals, porches into 0x9ff4c200. No hardware writes. | 0x9ff13670 |
| 3 | DisplayPipe init (mode-dependent part: `0x3a201030`) | `0x3a20104c \|= 0x10`, bits[10:8] = 1, bits[26:16] = 0x400<br>**`0x3a201030 = (1280<<16)\|720 = 0x050002d0`**<br>`0x3a20205c = 0x1c001f0`, `0x3a202060 = 0x100`, `0x3a20105c = 0x400001` (tunables "default" @0x9ff47020)<br>`0x3a202068/78/88 = 0x1000`, `0x3a202064 = 0xbff00000` | 0x9ff014d4 |
| 4 | RGBOUT reset + size | `0x39600008 = 1`, wait for bit 0 to clear<br>`0x3960001c = 0`, `0x39600024 = 0`<br>**`0x39600040 = w-1 = 1279`**, **`0x39600034 = h-1 = 719`**<br>`0x39600004 = 0x23` (RGB; iOS uses the same value for its RGB case @0x80c25b38)<br>**`0x3960006c = [0x24]+[0x2c]-2 = 8`** | 0x9ff16054 |
| 5 | Framebuffer allocation | stride = `(w*32/8 + 63) & ~63` = 5120<br>size = page-align(stride*h) = 0x384000<br>region = max(3*size, size+0xfc000) = 0xa8c000, placed top-down from 0x9fffc000<br>**base = 0x9f570000**, which is the ADT `/vram` node and the Linux `fb_region` | 0x9ff02af8 |
| 6 | Plane 0 (plane n at `+0x1000*n`) | `0x3a204040 = 1 \| fmt<<8` (fmt 0 = ARGB8888, 4 = RGB565)<br>**`0x3a204044 = fb base`**<br>**`0x3a204048[31:6] = stride`** (RMW)<br>`0x3a20404c = 0`<br>**`0x3a204050 = (w<<16)\|h`**<br>`0x3a204054 = 0`<br>**`0x3a204058 = (w<<16)\|h`**<br>`0x3a20405c = 1`<br>`0x3a204060/80/84/90 = 0`<br>`0x3a204088 = 0x3a20408c = 0x100000` (scale 1.0, GUESS 12.20 fixed point)<br>**`0x3a204070 = (w<<16)\|h`**<br>`0x3a202040 = 0xffff0202` | 0x9ff015a4 |
| 7 | Pipe enable | `0x3a201038 \|= 0x100`. iOS treats this bit as "iBoot left the display on" (READ @0x80c12f58). | 0x9ff01674 |
| 8 | HDMI start (display_type 7) | gate 0x50<br>`CTRL+0x30 &= ~1` (PHY power-off-enable cleared)<br>`CTRL+0x74` bit 0 pulsed 1 → 0 (PHY_RSTOUT)<br>`CORE+0 &= ~0x10`<br>bridge init (section 1.2)<br>EDID read, then `configure_video` | 0x9ff1466c, 0x9ff1371c |
| 9 | `configure_video` | section 2.1 | 0x9ff14bb0 |
| 10 | RGBOUT enable | `0x39600000 = 9` | 0x9ff160b8 via 0x9ff02c00 |
| 11 | Gamma | 3 × 256 entries: `0x39600054 = (cur&0xfffe0f00)\|i\|0x14000/0x15000/0x16000`, then `0x39600058 = lut[i]&0xfffff` (LUT @0x9ff453a8, **not** a linear ramp)<br>then `0x3960004c = 9` | 0x9ff160e0 |

### 2.1 `configure_video` (0x9ff14bb0), in order

Notation: `C+x` = CORE 0x39710000+x and `T+x` = TG 0x39750000+x. "(2)"
means a 16-bit value across x and x+4. In the formulas, `cfg` is the HDMI
config with CEA slot meaning: `fp`, `sw`, `bp`, `total`, `active` per axis.

1. Look up the PHY row for the pixel clock (0x9ff14aa4 → 0x9ff152d4) and
   check depth and space.
2. Read `hdmi_or_dvi` = the EDID result at 0x9ff473a0 (1 = HDMI, 2 = DVI,
   -1 = no EDID).
3. CORE timing:
   - `C+0xa0`(2) H_BLANK = htotal−hact.
   - `C+0xb8`(2) V1_BLANK = vtotal−vact.
   - `C+0xb0`(2) V2_BLANK = vtotal.
   - `C+0xc0`(2) V_LINE = vtotal.
   - `C+0xc8`(2) H_LINE = htotal.
   - `C+0xe8` INT_PRO_MODE = interlaced.
   - `C+0x110`(2) V_BLANK_F0 = vtotal+vblank+1.
   - `C+0x118`(2) V_BLANK_F1 = vtotal.
   - `C+0xe0` HSYNC_POL = !hpol.
   - `C+0x120`(2) H_SYNC_START = hfp−2.
   - `C+0x128`(2) H_SYNC_END = hfp−2+hsw.
   - `C+0xe4` VSYNC_POL = !vpol.
   - `C+0x138`(2) = vfp.
   - `C+0x130`(2) = vfp+vsw.
   - `C+0x148`(2) = vtotal+vfp.
   - `C+0x140`(2) = vtotal+vfp+vsw.
   - `C+0x158`(2) and `C+0x150`(2) = hfp+htotal/2.
4. `T+0x00` TG_CMD = 0 (progressive). This stores a byte, so **TG_EN is
   off** during configuration.
5. TG:
   - `T+0x18`(2) H_FSZ = htotal.
   - `T+0x20`(2) HACT_ST = htotal−hact.
   - `T+0x28`(2) HACT_SZ = hact.
   - `T+0x30`(2) V_FSZ = vtotal.
   - `T+0x38`(2) VSYNC = 1.
   - `T+0x40`(2) VSYNC2 = vtotal+1.
   - `T+0x48`(2) VACT_ST = vtotal−vact.
   - `T+0x50`(2) VACT_SZ = vact.
   - `T+0x58`(2) FIELD_CHG = vtotal+1.
   - `T+0x60`(2) VACT_ST2 = vblank+vtotal+1.
   - `T+0x78`(2) VSYNC_TOP_HDMI = 1.
   - `T+0x80`(2) VSYNC_BOT_HDMI = vtotal+1.
   - `T+0x88`(2) FIELD_TOP_HDMI = 1.
   - `T+0x90`(2) FIELD_BOT_HDMI = vtotal+1.
6. Colour setup (0x9ff156d0):
   - `C+0x00` bit 3 for YCbCr422;
   - blue-screen colour `C+0xd520..0xd534`;
   - limited range: `C+0x60 = 0xeb`, `C+0x64 = 0x10`, `C+0x68 = 0xf0`,
     `C+0x6c = 0x10`;
   - `C+0x04` bits [6:5];
   - depth in `C+0xd00` bits [1:0].

   Then `C+0xd04` bit 0 = test_mode. None of this depends on the mode.
7. 0x9ff15688:
   - `C+0x40` MODE_SEL[1:0] = 2 if `hdmi_or_dvi == 1`, otherwise 1 (DVI);
   - `C+0x08 &= 0xdd`, and `|= 0x22` if DVI.

   **So iBoot ends in HDMI mode only if it parsed an EDID that says HDMI.**
8. PHY (0x9ff14f78..0x9ff14fba):
   - Build `buf[32]`: `buf[0]` = the current byte 0 of word 0x39780088 and
     `buf[1..30]` = the 30 config bytes of the row. `buf[31]` stays
     uninitialised.
   - iBoot also stores `(word 0x3978008c >> 3)` into `buf[7]` first, but the
     memmove (0x9ff3459c, `memmove(dst=sp+0x81, src=row, 30)`) overwrites
     it. That store is dead.
   - Write reg 0x1f = 0x00, then regs 0..31 = buf, then reg 0x1f = 0x80
     (mode set done).
   - Poll `CTRL+0x20` (PHY_STATUS_0) bit 0 until it is 1 (0x9ff157f0).
9. PHY TMDS power off (0x9ff1559c(0) → 0x9ff155f0(0x94)):
   - `v = (0x94 | (rd32(0x397800a4) >> 1)) & 0xff`;
   - write `v` into byte lane 1 of 0x397800a4 (PHY reg 0x1d).
   - The expression is copied literally. I cannot explain the `>> 1`.
10. `T+0x00 |= 1` (TG_EN).
11. If MODE_SEL reads DVI:
    - `C+0x00 |= 1` (SYS_EN);
    - sleep(100);
    - PHY power on: `v = (rd32(0x397800a4) >> 1) & 0x6b` into reg 0x1d;
    - wait for PHY ready.
12. If HDMI:
    - AVI InfoFrame: `C+0x710..0x718 = 82 02 0d`, `C+0x71c = 0xf3`
      (checksum), `C+0x720.. = 10 68 00 04 00 00…` (13 bytes, so **VIC 4**),
      then `C+0x700[1:0] = 2` (send every VSYNC). The infoframe is static at
      0x9ff473ec.
    - GCP: `C+0x210..0x218 = 0`, `C+0x214[2:0]` = depth code (0 for 8 bpc),
      `C+0x200 |= 0xc`, then `[1:0] = 2`.
    - AVMUTE set: `C+0x210 = (cur & 0xee) | 1`, sleep(one frame).
    - SYS_EN, sleep(100), PHY power on, wait ready, sleep(60).
    - AVMUTE clear: `C+0x210 = (cur & 0xee) | 0x10`.
13. Repeat step 6 and the `C+0xd04` write.

Sleep arguments go to iBoot's `task_sleep`, whose unit is µs (GUESS).

## 3. The 1080p values for every step

Timing: CEA-861 VIC 16, 1920x1080p60, 148.5 MHz.

| Axis | active | fp | sync | bp | total |
|---|---|---|---|---|---|
| H | 1920 | 88 | 44 | 148 | 2200 |
| V | 1080 | 4 | 5 | 36 | 1125 |

Both syncs positive. This is exactly what iOS started:

```
EventLog StartLink: H Active 1920 Total 2200 SyncWidth 44 BackPorch 148 FrontPorch 88
V Active 1080 Total 1125 SyncWidth 5 BackPorch 36 FrontPorch 4, SyncPolarity 1,
SyncRate 60.0, PreciseSyncRate 59.94
```

That is READ(live). iOS ran 1080p **59.94** (148.3516 MHz) in YCbCr444
limited range. For Linux, use 60.00 Hz with 148.5 MHz in RGB, which is what
iBoot's own table supports. The 59.94 Hz row is listed in section 3.3.

### 3.1 Display pipe and RGBOUT

| Register | 720p (iBoot) | 1080p | Source |
|---|---|---|---|
| `0x3a201030` output size `(w<<16)\|h` | 0x050002d0 | **0x07800438** | READ formula (iBoot 0x9ff01530; iOS 0x80c143a8 uses the same `bfi w,#16,#11`) |
| `0x3a204044` plane-0 base | 0x9f570000 | 0x9f570000 (unchanged) | READ |
| `0x3a204048[31:6]` stride (bytes) | 5120 (0x1400) | **7680 (0x1e00)**; write `(cur & 0x3f) \| 0x1e00` | READ (iBoot `bfi`; iOS reads it back `& ~0x3f` @0x80c12f7a) |
| `0x3a204050`, `0x3a204058`, `0x3a204070` | 0x050002d0 | **0x07800438** | READ formula |
| `0x3a204088/408c` scale | 0x100000 | 0x100000 (unchanged) | READ |
| `0x39600040` width−1 | 1279 | **1919 = 0x77f** | READ (iBoot, and iOS @0x80c25aaa `DisplayWidth−1`) |
| `0x39600034` height−1 | 719 | **1079 = 0x437** | READ (iBoot, and iOS @0x80c25aca) |
| `0x3960006c` | 8 | **39 = 0x27** (see the note below) | READ formula; the value is derived |

Note on `0x3960006c`:

- iBoot writes `t[0x24] + t[0x2c] − 2`.
- In iBoot's real 720p signal, `t[0x24]` is the **vertical back porch** (5)
  and `t[0x2c]` is vsync (5): see the porch trap in section 2.
- iOS computes this register from two vertical-axis timing getters on the
  HDMI interface, minus 2 (READ @0x80c259c0..d8). I did not identify which
  two getters.
- So the register is most likely `vbp + vsw − 2`, which gives 36+5−2 = **39**
  for 1080p. That is a GUESS on the meaning.
- The alternative reading `vfp + vsw − 2` gives **7**.
- Try 39 first. If the top of the picture is torn or unstable, try 7.

Framebuffer:

- stride 7680, size 7680*1080 = 0x7e9000.
- 0x9f570000 + 0x7e9000 = 0x9fd59000, which is inside the reserved
  `fb_region` 0x9f570000+0xa8c000 (`dts/j33iap.dts`) and below iBoot's
  0x9ff00000. No DT change is needed for the experiment.
- The `simple-framebuffer` node can be changed to 1920/1080/7680 afterwards.
- iBoot's 0xa8c000 region is exactly 3 × the 720p buffer (READ: the allocator
  above). The iOS ADT still says `/vram` 0x9f570000+0xa8c000 while iOS runs
  1080p from its own surfaces (READ(live)).

### 3.2 HDMI CORE and TG (READ(live), cross-checked against iBoot's formulas)

Every value below is the iBoot formula from section 2.1 applied to the 1080p
timing above. The **live iOS snapshot holds exactly these values** (decoded
from `RegisterSets`, CORE and TG sets).

| Reg | 720p as iBoot writes it (prediction for the survey) | 1080p | Snapshot |
|---|---|---|---|
| C+0xa0/a4 H_BLANK | 370 = 0x172 | 280 = 0x118 | 0x18 0x01 ✓ |
| C+0xb8/bc V1_BLANK | 30 | 45 = 0x2d | 0x2d ✓ |
| C+0xb0/b4 V2_BLANK | 750 = 0x2ee | 1125 = 0x465 | 0x65 0x04 ✓ |
| C+0xc0/c4 V_LINE | 750 | 1125 | ✓ |
| C+0xc8/cc H_LINE | 1650 = 0x672 | 2200 = 0x898 | ✓ |
| C+0x110/114 | 781 = 0x30d | 1171 = 0x493 | ✓ |
| C+0x118/11c | 750 | 1125 | ✓ |
| C+0xe0 / C+0xe4 HSYNC/VSYNC_POL | 0 / 0 | 0 / 0 | 0 ✓ |
| C+0xe8 INT_PRO_MODE | 0 | 0 | ✓ |
| C+0x120/124 H_SYNC_START | **218 = 0xda** (porch swap) | 86 = 0x56 | ✓ |
| C+0x128/12c H_SYNC_END | 258 = 0x102 | 130 = 0x82 | ✓ |
| C+0x138/13c | 20 | 4 | ✓ |
| C+0x130/134 | 25 | 9 | ✓ |
| C+0x148/14c | 770 = 0x302 | 1129 = 0x469 | ✓ |
| C+0x140/144 | 775 = 0x307 | 1134 = 0x46e | ✓ |
| C+0x150/154, C+0x158/15c | 1045 = 0x415 | 1188 = 0x4a4 | ✓ |
| T+0x00 TG_CMD | 1 (TG_EN) | 1 | 0 in the snapshot: the link was stopped |
| T+0x18/1c H_FSZ | 1650 | 2200 = 0x898 | ✓ |
| T+0x20/24 HACT_ST | 370 | 280 = 0x118 | ✓ |
| T+0x28/2c HACT_SZ | 1280 = 0x500 | 1920 = 0x780 | ✓ |
| T+0x30/34 V_FSZ | 750 | 1125 | ✓ |
| T+0x38/3c VSYNC | 1 | 1 | ✓ |
| T+0x40/44 VSYNC2 | 751 = 0x2ef | 1126 = 0x466 | ✓ |
| T+0x48/4c VACT_ST | 30 | 45 | ✓ |
| T+0x50/54 VACT_SZ | 720 = 0x2d0 | 1080 = 0x438 | ✓ |
| T+0x58/5c FIELD_CHG | 751 | 1126 | ✓ |
| T+0x60/64 VACT_ST2 | 781 | 1171 = 0x493 | ✓ |
| T+0x78/7c VSYNC_TOP_HDMI | 1 | 1 | ✓ |
| T+0x80/84 VSYNC_BOT_HDMI | 751 | 1126 | ✓ |
| T+0x88/8c FIELD_TOP_HDMI | 1 | 1 | ✓ |
| T+0x90/94 FIELD_BOT_HDMI | 751 | 1126 | ✓ |

The snapshot also has non-zero registers that iBoot never writes:

- TG 0x68/0x70 = 0x47b/0x6ae. These are the constants mainline Exynos writes
  for VACT_ST3/ST4 (3D only).
- TG 0x98..0xb8, 0xd0, 0xd4.
- CORE 0xf0, 0x100..0x108.
- the CORE+0xd000 block (Apple CSC/dither for YCbCr).

Leave these at whatever iBoot left. They are not needed for 2D RGB 1080p
(GUESS).

AVI InfoFrame (HDMI mode only):

- Change PB4 (VIC) at `C+0x72c` from 0x04 to **0x10**, and the checksum at
  `C+0x71c` from 0xf3 to **0xe7**.
- Check: 0x82+0x02+0x0d+0x10+0x68+0x10 = 0x119, and 0x100−0x19 = 0xe7.
- Optional: set PB2 at `C+0x724` from 0x68 (ITU601) to 0xa8 (ITU709), as iOS
  does. The checksum then becomes **0xa7**.
- iOS's live AVI is `82 02 0d / 65 / 52 a8 00 10` (YCbCr444 + underscan).
  Its checksum 0x65 verifies, which confirms the PB layout (READ(live)).

Audio (not needed for video): if HDMI audio from `j33i-audio.md` is enabled
later, 48 kHz at 148.5 MHz uses N=6144, CTS=148500. ACR_CON=4 (measured
CTS) also tracks it automatically.

### 3.3 HDMI PHY bytes for 148.5 MHz

Source:

- **READ, twice:**
  - the iBoot PHY table row 87, at 0x9ff47414 + 87·100 = 0x9ff49610,
    pixel_clock 148500000;
  - the ADT `phy-cfg` row 86 (byte offset 0x2198), byte-identical.
- The iOS kext reads `phy-cfg` (`getProperty("phy-cfg")` @0x80a35516, stored
  at `this+0xcc`, searched by 0x80a39d9c). So these are the bytes iOS writes.
- **Not** from mainline. Mainline's `hdmiphy_v14_configs` 148.5 row is
  `01 d1 1f 00 40 40 f8 08 81 a0 ba d8 …`. Its PLL bytes 2-3 (`1f 00`) differ
  from this silicon's (`3e 15`), and the 74.25 rows diverge the same way.

PHY registers, with reg n = config byte n−1:

| Reg | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 74.25 (now) | keep | d1 | 3e | **35** | 40 | 20 | f8 | 04 | 81 | a0 | 73 | d9 | 45 | a0 | ac | 80 |
| 148.5 | keep | d1 | 3e | **15** | 40 | 20 | f8 | 04 | 81 | a0 | 73 | d9 | 45 | a0 | ac | 80 |

| Reg | 16 | 17 | 18 | 19 | 20 | 21 | 22 | 23 | 24 | 25 | 26 | 27 | 28 | 29 | 30 | 31 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 74.25 (now) | **07** | 80 | 11 | 04 | 02 | 22 | 44 | 82 | 54 | a5 | 24 | **01** | 00 | 00* | 01 | 0x80 |
| 148.5 | **0e** | 80 | 11 | 04 | 02 | 22 | 44 | 82 | 54 | a5 | 24 | **02** | 00 | 00* | 01 | 0→0x80 |

\* Reg 29 is the PHY power register. iBoot writes 0x00 from the table, then
toggles it with the step 9 and 11 expressions. Read its live value in the
survey.

The 74.25 row is iBoot row 37 (0x9ff47414 + 3700 = 0x9ff48288) and ADT row
36. The two are byte-identical (READ).

As 32-bit words at 0x39780088..0x397800a4, with `R0` meaning "keep the
current byte":

| Word | 74.25 MHz (expected readback) | 148.5 MHz |
|---|---|---|
| 0x39780088 | `0x353ed1R0` | `0x153ed1R0` |
| 0x3978008c | 0x04f82040 | 0x04f82040 |
| 0x39780090 | 0xd973a081 | 0xd973a081 |
| 0x39780094 | 0x80aca045 | 0x80aca045 |
| 0x39780098 | 0x04118007 | **0x0411800e** |
| 0x3978009c | 0x82442202 | 0x82442202 |
| 0x397800a0 | 0x0124a554 | **0x0224a554** |
| 0x397800a4 | `0x8001xx00` (reg 29 = xx) | `0x0001xx00` while writing, then `0x8001xx00` |

The 74.25 → 148.5 delta is three bytes:

- **reg 3: 0x35 → 0x15**;
- **reg 16: 0x07 → 0x0e**;
- **reg 27: 0x01 → 0x02**.

By analogy with mainline (C3 0x10→0x00, C27 0x01→0x03) these are the PLL
post-divider and the TMDS-clock range. The register meanings are a GUESS;
the bytes are READ.

1080p59.94 alternative (ADT row 85, 148351648 Hz; iBoot has no such row):

```
d1 3e 15 40 5b de 04 81 a0 73 d9 45 a0 ac 80 07 80 11 04 02 22 44 82 54 a6 24 02 00 00 01
```

(READ). Not recommended for the first experiment.

## 4. Smallest experiment from running Linux

Use `memrw ADDR [VALUE]` (`tools/memrw.c`). It does 32-bit access through
`/dev/mem`, one access per call. 32-bit writes to the one-byte CORE and TG
registers are what mainline Exynos does (`writel`). The PHY words must be
read-modify-write (READ: iBoot does RMW; iOS writes whole words).

Keep ssh up over Ethernet. HDMI is the only local console.

### Step 0: read-only survey (no risk)

This step validates this document before anything is written. Post the
values.

```sh
# DisplayPipe / RGBOUT
for a in 0x3a201030 0x3a201038 0x3a204040 0x3a204044 0x3a204048 0x3a204050 \
         0x3a204058 0x3a204070 0x39600000 0x39600004 0x39600034 0x39600040 0x3960006c; do memrw $a; done
# HDMI CTRL / CORE / TG
for a in 0x39700020 0x39700030 0x39710000 0x39710040 0x39710120 0x39710124 \
         0x3971071c 0x3971072c 0x39750000 0x39750018 0x3975001c; do memrw $a; done
# PHY bridge
for a in 0x39780084 0x39780088 0x3978008c 0x39780090 0x39780094 0x39780098 \
         0x3978009c 0x397800a0 0x397800a4; do memrw $a; done
```

Expected, if this RE is right:

| Register | Expected | Why |
|---|---|---|
| `0x3a201030` | 0x050002d0 | |
| `0x3a201038` | bit 8 set | |
| `0x3a204044` | 0x9f570000 | |
| `0x3a204048 & ~0x3f` | 0x1400 | |
| `0x39600040` | 0x4ff | |
| `0x39600034` | 0x2cf | |
| `0x3960006c` | 8 | |
| `0x39700020` | bit 0 = 1 | PHY ready |
| `0x39710120` / `0x39710124` | **0xda / 0x00** | proves the porch-swap reading |
| `0x39750018` / `0x3975001c` | 0x72 / 0x06 | H_FSZ = 1650 |
| PHY words | the "74.25" column of section 3.3 | proves the PHY layout |
| `0x39710040 & 3` | 2 = HDMI, 1 = DVI | decides whether to touch the AVI |

**If the PHY words do not match the 74.25 column, stop.** The PHY model is
wrong.

### Step 1: paint a 1080p test pattern into the buffer

Use `/dev/fb0` mmap. simplefb's `smem_len` is 0xa8c000, which is at least
0x7e9000. Use stride 7680: for example, four colour bars plus a 1-pixel
white border.

First stop fbcon from overwriting it:

```sh
echo 0 > /sys/class/graphics/fbcon/cursor_blink
```

A garbled fbcon after the switch must not be misread as "no signal".

### Step 2: the switch

Run it as one script with no pauses. It is reversible by reboot.

```sh
w() { memrw "$1" "$2" >/dev/null; }
r() { memrw "$1" | awk '{print $3}'; }
busy() { i=0; while [ $(( $(r 0x39780084) & 1 )) -ne 0 ] && [ $i -lt 1000 ]; do i=$((i+1)); done; }
# a) stop TG and SYS_EN (mainline hdmi_start(false); iBoot step 4)
w 0x39750000 0x0
w 0x39710000 $(( $(r 0x39710000) & ~1 ))
# b) PHY: mode-set enable (reg 31 = 0), 3 changed bytes, mode-set done (reg 31 = 0x80)
busy; w 0x397800a4 $(( $(r 0x397800a4) & 0x00ffffff ))
busy; w 0x39780088 $(( ($(r 0x39780088) & ~0xff000000) | 0x15000000 ))   # reg 3 = 0x15
busy; w 0x39780098 $(( ($(r 0x39780098) & ~0xff)       | 0x0e ))         # reg 16 = 0x0e
busy; w 0x397800a0 $(( ($(r 0x397800a0) & ~0xff000000) | 0x02000000 ))   # reg 27 = 0x02
busy; w 0x397800a4 $(( $(r 0x397800a4) | 0x80000000 ))
busy
usleep 10000   # bit 0 is still 1 from the 74.25 lock; mainline sleeps 10-12 ms before polling
i=0; while [ $(( $(r 0x39700020) & 1 )) -eq 0 ] && [ $i -lt 1000 ]; do i=$((i+1)); done
echo "pll wait $i, PHY_STATUS_0 $(r 0x39700020), PHY_STATUS_PLL $(r 0x39700028)"   # snapshot: both 1
# c) CORE timing (lo, hi)
w 0x397100a0 0x18; w 0x397100a4 0x01      # H_BLANK 280
w 0x397100b8 0x2d; w 0x397100bc 0x00      # V1_BLANK 45
w 0x397100b0 0x65; w 0x397100b4 0x04      # V2_BLANK 1125
w 0x397100c0 0x65; w 0x397100c4 0x04      # V_LINE 1125
w 0x397100c8 0x98; w 0x397100cc 0x08      # H_LINE 2200
w 0x39710110 0x93; w 0x39710114 0x04      # 1171
w 0x39710118 0x65; w 0x3971011c 0x04      # 1125
w 0x39710120 0x56; w 0x39710124 0x00      # H_SYNC_START 86
w 0x39710128 0x82; w 0x3971012c 0x00      # H_SYNC_END 130
w 0x39710138 0x04; w 0x3971013c 0x00      # 4
w 0x39710130 0x09; w 0x39710134 0x00      # 9
w 0x39710148 0x69; w 0x3971014c 0x04      # 1129
w 0x39710140 0x6e; w 0x39710144 0x04      # 1134
w 0x39710150 0xa4; w 0x39710154 0x04      # 1188
w 0x39710158 0xa4; w 0x3971015c 0x04      # 1188
# d) TG timing
w 0x39750018 0x98; w 0x3975001c 0x08      # H_FSZ 2200
w 0x39750020 0x18; w 0x39750024 0x01      # HACT_ST 280
w 0x39750028 0x80; w 0x3975002c 0x07      # HACT_SZ 1920
w 0x39750030 0x65; w 0x39750034 0x04      # V_FSZ 1125
w 0x39750040 0x66; w 0x39750044 0x04      # VSYNC2 1126
w 0x39750048 0x2d; w 0x3975004c 0x00      # VACT_ST 45
w 0x39750050 0x38; w 0x39750054 0x04      # VACT_SZ 1080
w 0x39750058 0x66; w 0x3975005c 0x04      # FIELD_CHG 1126
w 0x39750060 0x93; w 0x39750064 0x04      # VACT_ST2 1171
w 0x39750080 0x66; w 0x39750084 0x04      # VSYNC_BOT_HDMI 1126
w 0x39750090 0x66; w 0x39750094 0x04      # FIELD_BOT_HDMI 1126
#    (T+0x38, 0x78 and 0x88 stay 1)
# e) display pipe + RGBOUT
w 0x3a201030 0x07800438
w 0x3a204048 $(( ($(r 0x3a204048) & 0x3f) | 0x1e00 ))
w 0x3a204050 0x07800438; w 0x3a204058 0x07800438; w 0x3a204070 0x07800438
w 0x39600040 0x77f; w 0x39600034 0x437; w 0x3960006c 0x27
w 0x3a201038 $(( $(r 0x3a201038) | 0x100 ))
# f) AVI VIC (only if MODE_SEL read 2 in step 0)
w 0x3971072c 0x10; w 0x3971071c 0xe7
# g) restart
w 0x39750000 0x1
w 0x39710000 $(( $(r 0x39710000) | 1 ))
```

If `memrw` cannot take the `$(( ))` results (they print as decimal),
`printf 0x%x` them first.

This deliberately does **not**:

- pulse the PHY reset (`CTRL+0x74`);
- run iBoot's TMDS power toggles (reg 0x1d);
- reset RGBOUT (`0x39600008`).

The RGBOUT reset could wipe the gamma LUT, which is calibrated, not linear.
That is a GUESS.

### Verify

- ES02 capture: the snapshot JPEG should be **1920x1080**. Today it is
  1280x720. The colour bars should be straight, with the border on all four
  sides.
- Read back `0x39700020` bit 0 (PHY locked), `0x39750018/1c` = 0x98/0x08 and
  `0x3a201030`.
- The TV's info banner should say 1080p60.

### If it does not work, in this order

1. **No signal, PLL never locks** (`pll wait` hits 1000): pulse
   `0x39700074` bit 0 (1 then 0, as iBoot's start does), then redo b). Next
   try writing all 32 bytes, all eight words from section 3.3, the iOS way
   (iOS writes words). Mainline also resets the PHY before every mode set.
2. **Signal at 1080p but black or garbage**: the pipe registers probably did
   not latch. Clear and set `0x3a201038` bit 8, then `0x39600000 = 9`.
3. **Picture torn or rolling at the top**: try `0x3960006c = 7`.
4. **Sink says "no signal" only in HDMI mode**: write the AVI checksum after
   PB4, or set AVMUTE around the switch as iBoot does (`C+0x210`: `|1` before,
   `(&0xee)|0x10` after).

### Making it permanent

Once the experiment works:

- Put the same sequence in a tiny early-boot driver, or a stage-2 shim before
  `simplefb` binds.
- Patch the `framebuffer@9f570000` node to `width 1920; height 1080;
  stride 7680`.
- Alternative (GUESS, untested): keep a 1280x720 buffer and let the plane
  upscale. Set `0x3a204070 = 0x07800438` (destination size) and
  `0x3a204088/8c = 0xaaaab` (1280/1920 in 12.20). The scale registers are
  READ, but their meaning is not.

## 5. Risks

- **A blank or out-of-range screen is the likely failure. The recovery is a
  reboot, which works.** Every register touched here is volatile and iBoot
  reprograms all of it on the next boot. Nothing is written to NAND, NVRAM or
  any OTP.
- HDMI is the only local console, so run the script over ssh. If Ethernet is
  down, the project's boot notes record that writing 0x213 to 0x3fa00070
  (SoC GPIO 28, iBoot's reset line) reboots from Linux. That is an anecdote,
  not re-verified here.
- A PHY at the wrong rate should not damage the TV (GUESS): TMDS is
  current-mode, and the 148.5 MHz row is iOS's own. The 0x1d power register
  toggle is not used. Partial PHY writes leave the PLL unlocked, which means
  no picture (GUESS: and no harm).
- If `0x39780084` busy never clears, stop writing to the bridge. Interleaving
  writes into a busy I2C transaction is the one way to leave the PHY in an
  odd state. A reboot recovers it.
- **Touching a gated block faults.** Every block used here is on, because
  the console is on it.
- The fbcon content is garbled after the switch until something redraws at
  stride 7680. That is expected.
- DRAM bandwidth for 1080p60 32 bpp is about 0.5 GB/s, the same load iOS
  carries. No risk is expected.

## Confidence

| Claim | Confidence | Basis |
|---|---|---|
| HDMI CORE/TG 1080p values | high | Live snapshot equals iBoot formulas |
| PHY bytes for 148.5 MHz | high | Two independent copies agree; kext loads `phy-cfg` |
| PHY write mechanics | high | iBoot and iOS both READ |
| Pipe sizes and stride | high | iBoot and iOS formulas |
| `0x3960006c = 39` | medium | Formula READ, meaning derived; fallback 7 |
| Live switch works without a full re-init | medium | GUESS on latch and reset requirements; ordered fallbacks given |
| No PMGR clock change needed | medium-high | Negative search plus clock-id value |

## Reproduction

- r2 on iBoot:
  ```sh
  r2 -a arm -b 16 -m 0x9ff00000 -e anal.armthumb=true -c 'aaa; pdf @ fcn.9ff1370c' iBoot.j33i.bin
  ```
  The `configure_video` body starts at 0x9ff14bb0. Note that 0x9ff34590 is
  an ARM-mode `bcopy`, and 0x9ff3459c is `memmove`.
- Kext disassembly:
  ```python
  j33i_kc.annotate(0x80c245c4, 0x80c27ad8)   # RGBOUT
  j33i_kc.annotate(0x80c0e080, 0x80c18954)   # DisplayPipe
  j33i_kc.annotate(0x80a350e8, 0x80a3e9cc)   # HDMITX
  ```
- PHY tables:
  - iBoot: 100-byte rows from file offset 0x47414 (101 rows, 23.75..162 MHz).
  - ADT: the `phy-cfg` bytes in `j33i-full.dts` (10000 bytes, 100 rows).
- Live snapshot: parse `RegisterSets` at `ioreg-service.txt:3713` as 4-byte
  little-endian words per set. The EventLog `StartLink` entries are on the
  same node.
