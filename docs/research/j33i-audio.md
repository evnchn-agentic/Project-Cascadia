# J33i audio: HDMI and optical S/PDIF

Static reverse engineering of how iOS 8.4.4 (12H847) on the Apple TV 3 Rev A
(AppleTV3,2, J33iAP, S5L8947X) gets sound out of HDMI and the optical port.
Nothing touched the device. The sources were:

- the decompressed kernelcache `work/kc.j33i.macho`,
- the ADT (`work/j33i-full.dts`), plus the **live** IORegistry dumps
  `work/ioreg-service.txt` and `work/ioreg-devicetree.txt`. These keep the
  `function-*` properties the DTS converter dropped, and they include a
  register snapshot of the HDMI TX,
- the iBoot image `work/iBoot.j33i.bin` (load base 0x9ff00000),
- the Linux 6.12 tree in `build/linux` (Exynos HDMI and Samsung ASoC),
- the P105 work in `p105-audio.md` (same SoC family and the same drivers).

Tags used below:

- **READ @addr**: seen in the disassembly or in a dump.
- **EMU**: computed by running iOS's own code in unicorn (the
  `p105-audio.md` method).
- **P105**: established on the iPad and assumed to carry over.
- **GUESS**: inferred.

All kernelcache addresses are virtual (12H847). All physical addresses are
`0x30000000 +` the ADT `reg`.

## 0. The answer in five lines

1. HDMI audio uses **I2S**. The I2S master at `0x34195000` generates BCLK and
   LRCK only, and has no FIFO and no DMA. I2S0..3 are clock **slaves**: each
   one is fed by CDMA and carries two channels. The I2S lines go to the HDMI
   TX's own I2S receiver. There is no i2s-switch and no external pad on this
   board. (READ: ADT/ioreg. The internal wiring is a GUESS.)
2. **The HDMI TX is the Samsung/Exynos "v1.4" HDMI IP.** It has the same
   sub-block windows and the same core offsets as
   `drivers/gpu/drm/exynos/regs-hdmi.h` (ACR at 0x400, AVI at 0x700, AUI at
   0x800, I2S receiver at +0x40000). iOS programs audio with exactly the
   register sequence that `exynos_hdmi.c` uses (READ).
3. **The S/PDIF block is the Samsung s5pv210 SPDIF**, register for register
   (`sound/soc/samsung/spdif.c`). The I2S block is **not** the mainline
   Samsung I2S. It is the older S5L8900 layout that P105's driver already
   programs.
4. DMA is the same **CDMA v3** as on P105 (`0x37000000`). I2S0 TX is channel
   0x17 on request line 1, and S/PDIF TX is channel 0x1f on request line 8.
5. MCLK comes from the PMGR **NCOs** with a 342 MHz reference: NCO0 drives
   the I2S master (at BCLK rate) and NCO4 drives S/PDIF (at 256 fs). iBoot's
   HDMI driver has **no audio code at all**. Neither path passes audio until
   Linux sets it up.

## 1. Kexts (READ, from `__PRELINK_INFO` and the per-kext Mach-O headers)

| Kext | Load | Size | Role |
|---|---|---|---|
| `com.apple.driver.AppleSamsungI2S` | `0x80a20000` | `0x4000` | `AppleSamsungI2SController`, which runs I2S master and slaves. `__text` 0x80a204a0+0x17ec. Vtable 0x80a220c8. |
| `com.apple.driver.AppleSamsungSPDIF` | `0x80c41000` | `0x4000` | `AppleSamsungSPDIF`, an IOAudio2 device named "S/PDIF". `__text` 0x80c41fd8+0xd3c. |
| `com.apple.driver.AppleSamsungHDMITX` | `0x80a34000` | `0x12000` | `AppleSamsungHDMITXController` plus `…S5L8947X` (S5L8947X vtable 0x80a41b98). `__text` 0x80a350e8+0x98e4. |
| `com.apple.iokit.AppleARMIISAudio` | `0x80c39000` | `0x8000` | `AppleARMIISAudioDevice` and `AppleARMDMAAudioDevice`: the generic IOAudio2 device over CDMA that SPDIF and the I2S nubs sit on. |
| `com.apple.driver.AppleARMPlatform` | `0x8049d000` | `0x47000` | `AppleARMIISController`/`AppleARMIISDevice` (strings at 0x804be4d6 and 0x804be4ec). The `audio-hdmi-*` children are `AppleARMIISDevice` nubs. |
| `com.apple.driver.AppleCDMA` | `0x80ac3000` | `0x7000` | `AppleCDMA`, `CDMAChannel` (cdma-version 3). |
| `com.apple.driver.AppleS5L8940X` | `0x80b32000` | `0xa000` | `AppleS5L8940XPerformanceController`. Its NCO `setFrequency` is at 0x80b35280. |
| `com.apple.iokit.IOAVFamily` | `0x8095e000` | `0x32000` | `IOAVIISAudioDMAEventSource`, the HDMI audio plumbing. |
| `com.apple.driver.AppleAC3Passthrough` | `0x80aac000` | `0x6000` | Non-PCM (AC-3) passthrough. Not needed for PCM. |
| `com.apple.iokit.IOAudio2Family`, `IOHDMIFamily` | `0x8094f000`, `0x80a24000` | | Frameworks. |

Live instance counts (IOKitDiagnostics) are AppleSamsungI2SController 5,
AppleSamsungSPDIF 1, AppleSamsungHDMITXControllerS5L8947X 1,
AppleARMDMAAudioDevice 1 and AppleAC3Passthrough 1. **AppleARMIISSwitch is 0,
and there is no AE2 node or class.** Unlike P105, J33i has no i2s-switch and
no AE2 clock muxes.

`AppleSamsungI2STXSerial` (5 instances) lives in `AppleSamsungSerial`
(0x80bd9000). It is a serial-over-I2S helper and is not on the audio path.
This is a GUESS from its name.

## 2. Topology (READ: live ioreg)

| Node | Phys | IRQ | Gate id | DMA (`dma-channels`: ch, FIFO, width/burst, request) | Notes |
|---|---|---|---|---|---|
| `i2s-master` | `0x34195000` | none | 114 | none | `function-mclk_frequency = <pmgr 'NCOf' 0>`, `i2s-slave = <audio-hdmi-0..3>` |
| `i2s0` | `0x34190000` | 99 | 115 | TX 0x17 `0x34190010` req 1; RX 0x18 `+0x38` req 0 | child `audio-hdmi-0` |
| `i2s1` | `0x34191000` | 100 | 116 | TX 0x19 req 3; RX 0x1a req 2 | `audio-hdmi-1` |
| `i2s2` | `0x34192000` | 101 | 117 | TX 0x1b req 5; RX 0x1c req 4 | `audio-hdmi-2` |
| `i2s3` | `0x34193000` | 102 | 118 | TX 0x1d req 7; RX 0x1e req 6 | `audio-hdmi-3` |
| `spdif` | `0x34194000` | 103 | 119 | TX 0x1f `0x34194010` req 8 | `function-mclk_frequency = <pmgr 'NCOf' 4>` |
| `hdmi` | `0x39700000` (9 windows) | 46, 47 | 107 | none | `audio = <audio-hdmi-master>` (phandle 0x40), `function-device_reset = <pmgr 'ARST' 0x52>` |
| `cdma` | `0x37000000` (0x2d000), `0x37800000` | 51..92 | (85) | | 42 channels and 42 IRQs. One IRQ per channel is a GUESS. |

Each `dma-channels` record is 16 bytes: channel, FIFO physical address,
transferControl, request line. transferControl is width in bytes in the low
half and burst in the high half: `0x00040002` means 16-bit and `0x00040004`
means 32-bit. Every port lists both. `dma-parent = 0x12` is the phandle of
`cdma` (the `IODMAController12000000` in ioreg). It is not an address.

The `audio-data` templates (`reg`, 32 bytes) are the same for all five
ports, except byte 0, which is 1 on the master and 0 on the slaves:
`01 00 00 00 02 00 01 00 00… 03 00 00 00 03 00 00 00 02 02 10 10`. That means
2 slots, TX/RX masks 3, 2 channels each way, 16-bit, rate filled in at run
time.

HDMI register windows. The ADT `reg` order equals the kext's "module"
index, `writeReg(module, off, val)` at 0x80a37a50 with bases at
`this+0xf0+4*module`. This is READ, and it is confirmed by the
`RegisterSetName`/`RegisterSetDeviceAddress` pairs in ioreg.

| Module | Phys | Name (ioreg) | Exynos equivalent |
|---|---|---|---|
| 0 | `0x39700000` | CTRL_BASE | `HDMI_CTRL_BASE` |
| 1 | `0x39710000` (+0xe000) | HDMI_CORE_BASE | `HDMI_CORE_BASE` (+0x10000) |
| 2 | `0x39730000` | SPDIF_BASE | `+0x30000` SPDIF receiver |
| 3 | `0x39740000` | I2S_BASE | `HDMI_I2S_BASE` (+0x40000) |
| 4 | `0x39750000` | TG_BASE | `HDMI_TG_BASE` |
| 5 | `0x39760000` | EFUSE_BASE | `HDMI_EFUSE_BASE` |
| 6 | `0x39770000` | CEC_BASE | CEC |
| 7 | `0x39717000` (0x200) | HDCP | core + 0x7000 |
| 8 | `0x39780000` | PHY / I2C bridge (GUESS; `writeI2CBridgeReg`, `HDMIPHYCON%u` strings) | |

## 3. The I2S block (READ + EMU)

The I2S block has the same register layout and the same driver logic as
P105's I2S1. `i2s-version = 3` on every port.

| Off | Name (P105) | Notes |
|---|---|---|
| 0x00 | CLKCON | bit 0 run. The "stopped" status is bit 1, which stop polls (READ @0x80a20a18). Stop writes 0x34 on v3 (READ @0x80a209e6). |
| 0x04 | TXCON | format, ratio, word size |
| 0x08 | TXCOM | `0x01feca06` = DMA request + interface on + watermark (READ @0x80a20e90..ec4, EMU) |
| 0x10 | TX FIFO | the CDMA target (ADT) |
| 0x30 / 0x34 / 0x38 | RXCON / RXCOM / RX FIFO | |
| 0x3c | STATUS | write 1 = flush |

Vtable `0x80a220c8` (READ): +0x340 power/stop (0x80a20968), +0x344 validate
format (0x80a20a94), **+0x348 configure (0x80a20d78)**, +0x34c slave
configure (0x80a21230), **+0x350 start (0x80a21408)**, +0x354 stop
(0x80a21460).

The validator (READ @0x80a20a94) accepts only these formats:

- `[4] == 2` slots and `[6] == 1`;
- TX/RX slot masks equal to 3;
- 2 channels, or 4/6/8 when `i2s-multi` is set (it is absent here);
- 16, 20 or 24 bits, from the mask 0x01110000.

Emulated with `tools/audio/j33i-i2s-emu.py`, which runs iOS's configure
and start. The
values below are what iOS writes. They have not been seen on hardware.

| Port | Format | Writes, in order |
|---|---|---|
| i2s-master (clock master) | 48 kHz, 16-bit | `STATUS=1, TXCON=0x03000081, RXCON=0x1000, TXCOM=0, RXCOM=0, CLKCON=1`; start: **NCO0 = 1 536 000 Hz** (= BCLK, 32 fs), `TXCOM=0x01feca06` |
| i2s-master | 44.1 kHz, 16-bit | same registers, NCO0 = 1 411 200 Hz |
| i2s-master | 48 kHz, 24-bit | `TXCON=0x030000c9, RXCON=0x1008`, NCO0 = 2 304 000 Hz (48 fs) |
| i2s0 (slave) | 48 or 44.1 kHz, 16-bit | `STATUS=1, TXCON=0x03100099, RXCON=0x1000, TXCOM=0, RXCOM=0, CLKCON=0x15` (slave, clocked from the bus); start: `TXCOM=0x01feca06` |
| i2s0 (slave) | 48 kHz, 24-bit | `TXCON=0x031000d9, RXCON=0x1008, CLKCON=0x15` |

The master's configure also calls each slave nub's +0x344/+0x34c to pass the
rate and sample size down (READ @0x80a20d86..dea). Those nubs are
`AppleARMIISDevice`, which were not emulated. The slave values above come
from calling the controller's configure directly with the slave template.

**Comparison with mainline** (`sound/soc/samsung/i2s-regs.h`: I2SCON 0x00,
I2SMOD 0x04, I2SFIC 0x08, I2SPSR 0x0c, I2STXD 0x10…): only the TX FIFO at
0x10 coincides. Bit meanings and the RX block (0x30..0x3c) differ, so
**samsung-i2s cannot be reused**. P105's `sound/soc/apple/s5l-audio.c`
already programs this exact I2S (its I2S1 MCLK path) and is the right base.

## 4. The S/PDIF block (READ @0x80c42624 start, @0x80c4277c stop)

| Off | Mainline `spdif.c` name | What iOS does |
|---|---|---|
| 0x00 | CLKCON | `= 1` (PWR_ON, internal MCLK, bit 2 = 0) last in start; `&= ~1` in stop |
| 0x04 | CON | `|= 0x20` (SW_RESET) first. Then `CON = (CON & 0xffc08a80) | pcm_width<<1 | 1`. pcm_width comes from the table @0x80c42fc8: 16→0, 20→1, 24→2. MCLKDIV = 0 (256 fs) and FIFO_TH = 0. |
| 0x08 | BSTAS | bit 0 set only for the `ac-3` format (non-PCM burst) |
| 0x0c | CSTAS | `(old & 0xc0000000) | rate | (nonPCM<<1)`. rate: 48k `0x02000804`, 44.1k `0x00000804`, 32k `0x03000804`, 96k `0x0a000804`. That is category 0x08, NO_COPYRIGHT, and the IEC958 Fs field in bits 24-27. |
| 0x10 | DATA_OUTBUF | the CDMA FIFO (ADT) |

The order in start is:

1. `function-mclk_frequency(256·fs)`, which sets NCO4 to 12.288 MHz at 48 kHz.
2. `pclk_gate` (absent on J33i).
3. The provider clock on (gate 119).
4. The CON reset, CON, BSTAS, CSTAS.
5. CLKCON = 1.

Supported rates are 32, 44.1, 48 and 96 kHz (READ @0x80c425c0).

**This matches mainline `spdif.c` register for register** (CLKCON/CON/
BSTAS/CSTAS/DATA_OUTBUF and the CON_PCM, SW_RESET, MCLKDIV and CSTAS field
positions). There are two differences. Mainline sets FIFO_TH = 7 and
USERDATA_23RDBIT, and iOS clears both (GUESS: harmless). Mainline also uses
category CDP (0x01) where iOS uses 0x08.

**Reuse:** `spdif.c` has no OF match table (it uses s3c platform data). It
needs the clocks `spdif` and `sclk_spdif` and a Samsung-style dmaengine. So
mainline is reusable only after adding a compatible, a clock provider for
NCO4 and a CDMA dmaengine. Copying its 100 lines of register code into an
`s5l-audio.c`-style driver is cheaper.

## 5. DMA (CDMA v3)

The J33i `AppleCDMA` has the same error strings as P105's ("CDMA ERROR on
channel %d interrupt 0x%x csr 0x%x mar 0x%x car 0x%x dbr 0x%x dcr 0x%x dar
0x%x", READ @0x80ac6cc1). The channel stride is `ch << 12` (READ in
`cdma.s`, e.g. 0x80ac4c62). The ADT has `cdma-version = 3`. J33i iBEC/iBoot
contain the same `cdma_init()` / `kCDMAError*` code. So the P105 description
applies:

- channel `n` is at `0x37000000 + 0x1000·n`;
- the registers are CSR, DCR (request line in bits 16-21, width in bits 2-3,
  burst in bits 4-6; **bit 0 locks DCR until reboot**), DAR (24-bit), DBR,
  MAR and CAR;
- a command is 32 bytes `{NXT, CMD = 3|2<<24, ADDR, LEN}`;
- a ring self-paces on the request line;
- the global clock-enable bits are at +0x00/+0x04.

This is P105 knowledge. It has not been re-read on J33i.

| Use | Channel | Request (DCR[21:16]) | DAR (low 24 bits) | Width |
|---|---|---|---|---|
| HDMI L/R (i2s0 TX; i2s0 → CH0 is a GUESS) | **0x17** | **1** | `0x190010` | 2 or 4 bytes |
| HDMI ch 3-8 | 0x19 / 0x1b / 0x1d | 3 / 5 / 7 | `0x191010` / `0x192010` / `0x193010` | |
| S/PDIF TX | **0x1f** | **8** | `0x194010` | 2 or 4 bytes |

Note that channel 0x17 is MCA0's channel on P105 but I2S0's on J33i (READ:
J33i ADT).

From the emulation (EMU), the I2S slave's configure tells its DMA channel
the width code (`+0x88`: 0 for 16-bit, 2 for 24-bit) and a block size
(`+0x80`). The register meaning of those calls is P105's DCR description.

## 6. Clocks

PMGR gates, decoded from the 44-byte `device-clocks` entries of the J33i
ADT (READ; the iPad's entries are 36 bytes):

| Name | Id | Register | Parent |
|---|---|---|---|
| AUDIO-CLK | 21 | clock `0x3f100058` | PREDIV0..3 |
| AUDIO | 46 | power `0x3f10109c` | AUDIO-CLK |
| I2S | 47 | power `0x3f1010a0` | AUDIO. One gate for **all** I2S ports. |
| I2S-MASTER, I2S0..3 | 114..118 | none (virtual) | I2S |
| SPDIF | 119 | power `0x3f1010b0` | AUDIO |
| MCA0 | 113 | power `0x3f101140` | AUDIO (no MCA node on J33i) |
| HDMI | 107 | power `0x3f101148` | 44, 52 |
| CDMA | 85 | power `0x3f101078` | |
| NCO_REF0 / NCO_REF1 | 121 / 122 | clock `0x3f1000c0` / `0x3f1000c4` | PREDIV0..3 |

- **NCO reference = 342 MHz.** This is READ, not measured: the arm-io
  `clock-frequencies[0x3c]` is 342000000, and the pmgr's `clock-ids` are
  `<316 317>` = 0x13c/0x13d. It matches P105's measured value.
- **NCO programming is identical to P105** (READ @0x80b35280..0x80b353a4):
  - there are seven NCOs (n ≤ 6), at `PMGR + 0x110 + 16·n`;
  - CTRL = `0x90000000`, wait for bit 30;
  - `+4 = 2·Hz`, `+8 = 2·Hz − Fref` (Fref from clock gate 0 = NCO_REF0),
    waiting on bit 9 after each;
  - CTRL `|= 0x400`, then `|= 0xc00`;
  - 0 Hz → CTRL = 0, and NCO_REF0 is gated off when all seven are 0.
- Index mapping (READ, ioreg `function-mclk_frequency` args): **i2s-master →
  NCO0 (`0x3f100110`)** and **spdif → NCO4 (`0x3f100150`)**.
- Values iOS asks for:
  - I2S master: BCLK = 32 fs at 16-bit (1.536 MHz at 48k, 1.4112 MHz at
    44.1k) and 48 fs at 24-bit, with `TXCON` bit 7 "MCLK = BCLK" (EMU).
  - S/PDIF: 256 fs (12.288 / 11.2896 MHz), READ @0x80c42642..65e.
- **Open:** on P105 the NCO reached a port only through AE2's MCS mux. J33i
  has no AE2, so the NCO→port path is presumably fixed wiring (GUESS). If an
  experiment shows no clock, this is the first suspect.

## 7. HDMI audio (READ from `AppleSamsungHDMITX`, cross-checked against a live snapshot)

The S5L8947X subclass selects the register-offset table at **0x80a40fb4**
(READ @0x80a3d9b8). The 8747X subclass uses the v1.3 table at 0x80a40f68.
Entries, with Exynos v1.4 names:

- +0x00 AUDIO_CLKSEL **0x070** (CTRL)
- +0x0a AVI_CON 0x700
- +0x1a **AUI_CON 0x800**, AUI_HEADER0 0x810, AUI_BYTE1 0x820, AUI_CHECK_SUM
  0x81c
- +0x32 GCP_CON 0x200
- +0x3a **ASP_CON 0x300**, ASP_SP_FLAT 0x304, ASP_CHCFG0 0x310
- +0x40 **ACR_CON 0x400**, MCTS0 0x410, CTS0 0x420, N0 0x430, and 0x440

All of them equal `regs-hdmi.h` v1.4.

`startAudioLink` (0x80a3a8d0) runs this sequence (READ):

1. Skip everything if `CORE+0x40` (MODE_SEL) `& 3 == 1`, i.e. DVI mode.
2. `configureAudioTransmitter` (0x80a3aaec). On the `audio-hdmi-master` nub:
   setSampleSize, setChannelCount(2, n) and setLRCLKFrequency. This
   configures the I2S master and slaves of section 3.
3. Audio source 1 (I2S) is selected because an `audio` provider exists
   (READ @0x80a35872; `default_audio_source` can override it). Then
   **CTRL+0x70 `&= ~1`**.
4. I2S receiver (0x80a3ac34), at `HDMI_I2S = 0x39740000`:
   - PIN_SEL_0..3 (0x0c..0x18) take a value per field from a per-SoC table.
     On S5L8947X it is **7 for LRCK, SCLK, SDATA0..3 and DSD** (table
     0x80a40f18 via +0x3e8 → 0x80a3e5a4). Exynos/8747X use 6, 5, 4, 3, 2, 1, 0
     (0x80a40f4c).
   - CON_1 (0x04) `&= ~3`.
   - CON_2 (0x08): bits 2-3 sample size (16→1, 20→2, 24→3), bits 4-5 = 0 for
     16-bit and 1 (48 fs) for 20/24-bit, bit 6 cleared.
   - CLK_CON (0x00) `|= 1`.
5. `configureI2SReceiver` (0x80a3ae18): CH_ST 0x28..0x38 gets five channel
   status bytes, then CH_ST_CON (0x24) `|= 1` (reload).
6. `configureI2SMUX` (0x80a3ae5c):
   - MUX_CH (0x54) = OR of `1 << map[i]` over the channels, with map = 0..7,
     so **0x03 for stereo**;
   - MUX_CUV (0x58) = 3;
   - MUX_CON (0x20): bits 5-7 = 3, bits 2-3 = source (I2S → 1), bit 1 = 1
     for I2S;
   - then MUX_CON `|= 0x11`.

   The result is **0x77**, which matches Exynos's `IN_DISABLE|AUD_I2S|CUV_I2S|
   MUX_ENABLE` = 0x17 plus the noise filter in bits 5-6.
7. ACR (0x80a3af64): N and CTS come from an imported helper (stub
   0x80a3e9cc).
   - Write N to 0x430/434/438, LSB first.
   - If CTS ≠ 0: write CTS to 0x420/424/428, write a byte to 0x440, set
     ACR_CON bits 3-4 from the helper, and set **ACR_CON[2:0] = 3**
     (CTS fixed).
   - Otherwise, if `N & 0x7f == 0`: set ACR_CON[2:0] = **4** (measured CTS).
     That is what Exynos `hdmi_reg_acr` does.
8. The audio device is started (provider vt+0x3ec, which feeds the DMA).
9. ASP (0x80a3b094):
   - ASP_CON[3:0] = the channel-pair mask, 0 for 2 channels; bit 4 set for
     layout 1 when there are more than 2 channels; bits 5-6 cleared.
   - SP_FLAT (0x304) = 0.
   - ASP_CHCFG0..3 (0x310..0x31c) = **0x08, 0x1a, 0x2c, 0x3e**.
   - **CON_0 (0x000) `|= 4`** (ASP_EN).
10. The audio InfoFrame type 0x84 goes through `startInfoFrameGated`
    (0x80a3b26c): header to 0x810..0x818, checksum 0x81c, bytes from 0x820,
    then **AUI_CON[1:0] = 2** (every VSYNC).

Stop (0x80a38584) undoes all of this: ASP_EN off, AUI stopped, ACR_CON
`&= ~7`, MUX_CON `&= ~0x11`, I2S CLK_CON `&= ~1`. A "fake audio link"
(`startFakeAudioLink` 0x80a384e0) only stores the format and fakes DMA
completions for clients when no real audio is running. It writes no
registers.

**Live snapshot.** `RegisterSets` in `ioreg-service.txt` line 3713,
decoded as 4-byte little-endian words, was taken with the
link idle: CON_0 = 0x20 (blue screen, HDMI_EN off) and TG_CMD = 0. It still
holds the last audio setup, which agrees with the reading above:

- I2S CON_2 = 0x1c (24-bit, 48 fs);
- PIN_SEL = 0x77/0x77/0x77/0x07;
- MUX_CON = 0x66 (0x77 with the 0x11 cleared by stop);
- MUX_CH = 3, MUX_CUV = 3;
- CH_ST = 04 01 00 02 db (48 kHz);
- ASP_CHCFG = 08 1a 2c 3e;
- N = 0x0016c0 = 5824 and CTS = 0x022551 = 140625. That is the HDMI-spec
  pair for **48 kHz at 148.5/1.001 MHz** (1080p59.94, what iOS runs).
- AUI header 84 01 0a, checksum 0x70, byte1 = 0x01 (2 channels);
- AVI VIC 16;
- ACR_CON, AUI_CON and ASP_CON = 0 (stopped).

**Does the iBoot-initialised TX pass audio? No (READ + GUESS).**

- iBoot's HDMI driver has 41 `hdmi_*` strings (count from STATUS.md:
  `configure_video`,
  `prepare_video_link`, `configure_PHY`, `init_video`, …). It contains no
  audio, ACR, InfoFrame or I2S string.
- The only HDMI address literal is the base 0x39700000 (@0x9ff4538c).

So nothing sets ASP_EN, ACR or AUI. Whether iBoot leaves MODE_SEL at HDMI or
DVI is **unknown**. If it is DVI, iOS's own code refuses audio, and Linux
must switch to HDMI mode first. That switch can blank the picture once.

## 8. The smallest experiments

Every step uses `devmem`/`audmmio`-style MMIO from userspace under the
running Linux.

**Step 0: read-only survey (no risk, answers the open questions).** Read the
following and post the values:

- PMGR `0x3f10109c` (AUDIO), `0x3f1010a0` (I2S), `0x3f1010b0` (SPDIF),
  `0x3f100058` (AUDIO-CLK), `0x3f1000c0` (NCO_REF0), NCO0 `0x3f100110..1c`,
  NCO4 `0x3f100150..5c`;
- HDMI CTRL `0x39700070`, CORE `0x39710000` (CON_0), `0x39710040`
  (MODE_SEL), `0x39710300`, `0x39710400`, `0x39710700`, `0x39710800`;
- TG `0x39750000` and `0x39750018..2c` (timing, which gives the pixel clock
  for N/CTS);
- I2S receiver `0x39740000..0x58`;
- CDMA `0x37000010/14` (channel clocks).

Only read windows whose PMGR gate reads on. The Arasan note warns that
touching a gated block faults. HDMI is necessarily on, because the console
is on it.

**Step 1: S/PDIF, the cheapest audible or visible signal.**

1. Power on AUDIO, then SPDIF (P105 recipe `(v & ~0x10f) | 0xf`, wait for
   bits 7:4). Set AUDIO-CLK = `0x80000001` if it reads 0 (P105).
2. Set NCO4 to 12 288 000 Hz with the section 6 sequence.
3. S/PDIF: `CON |= 0x20`; `CON = 0x1`; `CSTAS = 0x02000804`; `CLKCON = 1`.
4. Signal: the Toslink port should **glow red**, because biphase-mark
   output toggles even on silence (a phone camera shows it). An optical
   receiver or AVR should lock at 48 kHz PCM.
5. Then write a quiet square wave to `0x34194010` from a paced loop at
   96 000 words/s. There is no known FIFO-level register (GUESS: DCNT 0x14).
   An underrun is only a click.

Risks: the TX pad's pinmux is not described anywhere in the ADT, so a dark
port may only mean the pad is still a GPIO. The NCO-to-SPDIF path is also
unverified (no AE2 on this SoC).

**Step 2: HDMI, PIO.**

1. Power on AUDIO (`0x3f10109c`), then I2S (`0x3f1010a0`). Set AUDIO-CLK =
   `0x80000001` if it reads 0. This step must not assume that step 1 ran.
2. I2S master as in section 3: NCO0 = 1.536 MHz,
   `TXCON=0x03000081, RXCON=0x1000, CLKCON=1, TXCOM=0x01feca06`.
3. i2s0 slave: `TXCON=0x03100099, CLKCON=0x15, TXCOM=0x01feca06`.
4. HDMI receiver: PIN_SEL `0x77, 0x77, 0x77, 0x07`; CON_1 = 0; CON_2 = 0x04
   (16-bit); CH_ST = `04 00 00 02 02`, then CH_ST_CON = 1; CLK_CON = 1;
   MUX_CH = 3; MUX_CUV = 3; MUX_CON = 0x77.
5. Clear CTRL+0x70 bit 0.
6. ACR: N = 6144 at 0x430/434/438; ACR_CON = 4 (measured CTS, so the exact
   iBoot pixel clock does not matter).
7. ASP_CON = 0; ASP_CHCFG = 08/1a/2c/3e; CON_0 `|= 4`.
8. AUI: header 84 01 0a, byte1 = 0x01, checksum; AUI_CON = 2.
9. If MODE_SEL reads DVI, set HDMI_EN (`MODE_SEL = 2`).
10. Feed `0x34190010`.

Signal: the TV/AVR info shows "PCM 2ch 48 kHz", then a tone.

"i2s0 lands on HDMI CH0/SDATA0" is a **GUESS** taken from the order of the
`i2s-slave` phandles. With PIN_SEL = 7 everywhere, nothing READ says which
slave reaches CH0. If step 2 is silent but the TV reports PCM, configure
i2s1..3 the same way and feed each in turn. That costs little and does not
risk the display.

Risks: changing MODE_SEL or CON_0 can drop or reset the picture, which the
only console uses. That is recoverable by reboot, so keep ssh or ethernet
up. Also, PIN_SEL = 7 is copied from iOS without being understood.

**DMA afterwards.** Reuse `s5l-audio.c`'s CDMA ring and its hrtimer MAR
position:

- S/PDIF: channel 0x1f, request 8, DAR 0x194010;
- HDMI: channel 0x17, request 1, DAR 0x190010.

Remember that DCR bit 0 latches until reboot.

## 9. Recommended order

1. Step 0 survey. It settles MODE_SEL, the gate states and the iBoot
   pixel clock.
2. S/PDIF PIO (step 1). It validates the power gates, the NCO and the pad
   with no display risk.
3. An S/PDIF ALSA card: an `s5l-audio.c` sibling with NCO4, the SPDIF
   registers from section 4 and CDMA 0x1f.
4. HDMI PIO (step 2), with MODE_SEL handled.
5. An HDMI ALSA card: I2S master and i2s0 slave from section 3, plus the
   HDMI audio register code. Port it from `exynos_hdmi.c`'s
   `hdmi_audio_config`, `hdmi_reg_acr` and `hdmi_audio_infoframe_apply`,
   with the PIN_SEL and MUX_CON values from section 7. HDMI can be an ASoC
   codec component on the existing simplefb.
6. Multichannel: i2s1..3 as more slaves with MUX_CH, ASP_CON and the CA
   field in the AUI. AC-3 passthrough comes last.

## Reproduction

In the repo (they need `capstone` and `unicorn` in a uv venv):

- `tools/audio/j33i_kc.py`: kernelcache segments, va→offset and an
  annotating Thumb disassembler. `KC_J33I` overrides the kernelcache path.
- `tools/audio/j33i-i2s-emu.py`: the section 3 table, printed by running
  iOS's I2S configure and start. Pass `[nclk_hz]`.

Only in the session scratchpad, so not durable:

- the HDMITX `writeReg` lister;
- the iBoot disassembler;
- the decoded HDMI snapshot (redo it by parsing `RegisterSets` at
  `ioreg-service.txt:3713` as 4-byte little-endian words per set);
- the annotated `.s` dumps.

All of them can be regenerated from `j33i_kc.py` and the addresses in this
document.
