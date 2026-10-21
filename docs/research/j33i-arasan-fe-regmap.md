# Arasan FE Ethernet MAC register map (AppleArasanFEController, iOS 8.4 Apple TV 3 kext)

Source: `~/atv3-linux/work/AppleArasanFEController.bin` (armv7 Thumb-2, load base 0x806b7000, `__text` 0x806b7ba8..0x806bda9c).
No LC_SYMTAB, so all names below are mine, derived from strings, vtable position and `IOLog`-style function-name arguments.
Static analysis only. Nothing was run.

Confidence tags:
- **[SEEN @addr]** = read directly in the disassembly at that address.
- **[INFERRED]** = deduced from context, not directly observed.
- **[UNKNOWN]** = observed, meaning not determined.

All addresses are virtual (0x806b7000 base). File offset = addr - 0x806b7000.

## 1. Accessors and object layout

Register base is `this+0xb0` [SEEN @0x806b8370: `str.w r6,[r4,#0xb0]`, r6 = IOMemoryMap::getVirtualAddress of `mapDeviceMemoryWithIndex(0)`, strings at 0x806be06a/0x806be0aa].
Every access is `ldr r0,[this,#0xb0]` then `ldr/str rX,[r0,#off]`, either inline or via these helpers:

| func | role | evidence |
|---|---|---|
| 0x806b9bc8 | `readReg(off)` | `ldr r0,[r0,r4]` @0x806b9be8 |
| 0x806b9768 | `writeReg(off,val)` | `str r4,[r0,r5]` @0x806b978a |
| 0x806b96b0 | `orReg(off,mask)` (read-modify-write) | @0x806b96d2..d6 |
| 0x806b96e0 | `andReg(off,mask)` | @0x806b9702..06 |

All of them are 32-bit accesses. No barriers, no `dmb`.
Every access is preceded by `if (this[0xbc] & 0x200) IOLog("AppleArasanFEController:readReg: offset %08lx\n", off)` (string 0x806bef91).
That bit is set in `_stopHW` (@0x806ba008) and cleared in `_prepareHW` (@0x806b8d9a), so the "log" is really a warning that a register was touched while the block is powered down. It is not a debug trace [INFERRED].
**Linux implication: never touch the MMIO space while the clock gate is off.**

Other `this` fields that matter:

| field | meaning | evidence |
|---|---|---|
| +0xa8 | provider (AppleARMIODevice) | 0x806b8324 |
| +0xb0 | MMIO base | above |
| +0xbc | state flags: 0x8 prepared, 0x10 enabled, 0x20 disabled, 0x200 hw-off | 0x806b8c32, 0x806bb47a, 0x806b92f6 |
| +0xc8..cd | MAC address bytes | 0x806b9404.. |
| +0xd0 | max RX pkt (default 0x5ee = 1518, property `max-rx-packet-size`) | 0x806b86e4 |
| +0xd4/0xd8/0xdc | RX ring: IOBufferMemoryDescriptor, phys, virt | 0x806bb238/0x806bb252/0x806bb246 |
| +0xe0/0xe4 | RX next-to-process index / count | 0x806bb3dc |
| +0xf8/0xfc/0x100 | TX ring: descriptor, phys, virt | 0x806baf60/0x806baf7a/0x806baf6e |
| +0x104/0x108/0x10c | TX produce index / clean index / in-flight count | 0x806bb0f6, 0x806bb1a4 |

## 2. Register map

DMA vs MAC split [SEEN @0x806b8a70..0x806b8ac6, strings 0x806bee6a "DMA Registers:" and 0x806bee9b "MAC Registers:"]: the dump routine `_regDump` (0x806ba054) is called with (0x00..0x40) labelled DMA, then (0x100..0x180), (0x1a0..0x1c8), (0x1e0..0x1e4) labelled MAC.

### DMA block (0x00..0x40)

| off | R/W | what the code does | confidence |
|---|---|---|---|
| 0x00 | W | `_prepareHW`: write 1 (0x806b8e76), write 0 (0x806b8e92), write 0x410 (0x806b8eb0). No delay between the first two, only the "unclocked access" check. Live reset value 0x08. Reading as DMA soft-reset(1)/release(0) then config 0x410: bits 4 and 10 meaning unknown. | writes SEEN; semantics INFERRED |
| 0x04 | RMW | DMA enable. Link up: `\|=3` (0x806ba5d6). Link down: `&=~3` (0x806ba392). KDP init: write 0 then `\|=3` (0x806bb5d2, 0x806bb690). Bit 0 and 1 presumably TX/RX DMA run, which one is which is unknown. | bits SEEN; split INFERRED |
| 0x08 | R/W | Interrupt status, write-back acks. ISR reads it, then writes the same value back (0x806b8756, 0x806b8774). Link-down also reads then writes back (0x806ba3cc, 0x806ba3e6). KDP acks with `writeReg(8,0x10)` (0x806bb6d6) and `writeReg(8,1)` (0x806bb58e). So write-1-to-clear. See bits in section 4. | SEEN; W1C INFERRED |
| 0x0c | W | Interrupt mask/enable. Link up: write 0x1f1 (0x806ba616). Link down: write 0 (0x806ba3b0). | SEEN |
| 0x10 | W | TX auto-fetch timeout in "ticks": `writeReg 0x10 = ticks` (0x806b917e). Log string "TX: Applying auto-fetch %d tick timeout" (0x806becbc). ticks = provider_clock_hz / 2000 (call to 64-bit udiv 0x806bd784 with divisor 0x7d0 @0x806b9112). Skipped if clock freq is 0. | SEEN; divisor/units INFERRED |
| 0x14 | W | **TX poll demand / kick**: `writeReg(0x14,1)` after queuing descriptors (0x806bb16a, KDP 0x806bb572). | SEEN |
| 0x18 | W | **RX poll demand**: write 1 after (re)arming an RX descriptor (0x806bb6ae, 0x806bb75c) and in the ISR when status bit 0x20 is set (0x806b87a8). | SEEN |
| 0x1c | W | **TX ring base, physical address** (`[this+0xfc]`) (0x806b90c2; KDP 0x806bb64a). | SEEN |
| 0x20 | W | **RX ring base, physical address** (`[this+0xd8]`) (0x806b90e0; KDP 0x806bb63e). | SEEN |
| 0x2c | W | RX interrupt coalescing: `0x80000000 \| (ticks << 7)` (bfi width 20 at bit 7) (0x806b9132..0x806b915a). Log text "RX: Applying %d packet threshold with %d tick timeout" where threshold is 0x40, but 0x40 is only passed to the log, not written. Live reset value 0x00ffff01. Skipped if clock freq is 0. | write SEEN; field meaning INFERRED |
| 0x40 | W/R | After the platform `function-device_reset` call, write 0x80000000 (0x806b8e04), then one read (0x806b8e20). Live reset value is already 0x80000000, so purpose unclear (a status bit it clears, or an enable). | SEEN; UNKNOWN meaning |

### MAC block (0x100..)

| off | R/W | what the code does | confidence |
|---|---|---|---|
| 0x100 | R/W | MAC control / speed-duplex. Init: write 0x18, read, write 0 (0x806b8ece, 0x806b8eec, 0x806b8f0c). On link up (`_linkChange` 0x806ba2f8) one of: 10H = 0, 10F = 4, 100H = 1, 100F = 5. Mapping of tbb cases to log strings at 0x806ba482/0x806ba4b0/0x806ba4de/0x806ba50c. So **bit0 = 100 Mbit, bit2 = full duplex**. | SEEN |
| 0x104 | RMW | MAC enable. Init write 8 (0x806b8f6e). Link up `\|=1` (0x806ba5b8), link down `&=~1` (0x806ba44e). KDP `\|=1`. TX vs RX unknown (one of 0x104/0x108 is TX, the other RX). | bit0 SEEN; role UNKNOWN |
| 0x108 | RMW | Same pattern as 0x104. Init write 0x18 (0x806b8f8e). Link up `\|=1` (0x806ba592), link down `&=~1` (0x806ba428). | bit0 SEEN; role UNKNOWN |
| 0x10c | W | `[this+0xd0] + 4` = 1522 (0x806b9062). Max RX frame length (incl. CRC) [INFERRED]. | write SEEN |
| 0x110 | W | 0x600 (0x806b9084) | SEEN; meaning UNKNOWN |
| 0x114 | W | 0x600 (0x806b90a2) | SEEN; meaning UNKNOWN |
| 0x118 | W/RMW | Init write 1 (0x806b903e). `setPromiscuousMode` (0x806b9650) does `\|= 0x100` / `&= ~0x100`. So bit 8 = promiscuous [INFERRED from vtable slot], bit 0 = filter/enable. | writes SEEN; names INFERRED |
| 0x120 | W | MAC addr bytes [1]:[0], i.e. `mac[0] \| mac[1]<<8` (0x806b8fc6..fca). | SEEN |
| 0x124 | W | `mac[2] \| mac[3]<<8` (0x806b8ff0..ff4). | SEEN |
| 0x128 | W | `mac[4] \| mac[5]<<8` (0x806b901a..01e). | SEEN |
| 0x150 | W | Multicast hash bits [15:0] (0x806b986a) | SEEN |
| 0x154 | W | hash bits [31:16] (0x806b988c) | SEEN |
| 0x158 | W | hash bits [47:32] (0x806b98aa) | SEEN |
| 0x15c | W | hash bits [63:48] (0x806b98c8) | SEEN |
| 0x1a0 | R/W | **MDIO (SMI) command/status**. See section 5. | SEEN |
| 0x1a4 | R/W | MDIO data (low 16 bits). | SEEN |
| 0x1a8 | R/W | RX statistics counter index/control: write `idx \| 0x8000`, poll bit 0x8000 clear (3 ms), value at 0x1ac. Valid idx 0..0x18. String "MAC_STAT_CTRL_BUSY never cleared". `_readCounter` 0x806ba6a8 (reads `base`, tests 0x8000 @0x806ba70c, writes `idx\|0x8000` via `bfi r6,#0x400,#5,#27` @0x806ba78a, reads `base+4` @0x806ba81e). `_counterDump` 0x806ba12c prints "Rx Counters" for base 0x1a8 (0x806ba1e4) and "Tx Counters" for base 0x1b4 (0x806ba174). | SEEN |
| 0x1ac | R | RX counter value | SEEN (base+4) |
| 0x1b4 | R/W | TX statistics counter index/control, same protocol. idx 0..0x0b. | SEEN |
| 0x1b8 | R | TX counter value | SEEN (base+4) |
| 0x1c0 | W | write 0x1f8 (0x806b8f4e). Unknown (within dump range 0x1a0..0x1c8). | SEEN; UNKNOWN |
| 0x1c4 | W | write 0x80 (0x806b8f2c). Unknown. | SEEN; UNKNOWN |
| 0x1e0 | R/W | Stat-counter overflow/interrupt status: ISR reads it and writes it back (0x806b87ce, 0x806b87ec) when status bit 0x100 is set. W1C [INFERRED]. | SEEN |
| 0x1e4 | R | included in the dump range only (0x806b8aba/ac2). Never accessed otherwise. | SEEN as range only |

Registers not listed were not seen in the code. The dump ranges also touch 0x24..0x3c, 0x12c..0x17c, 0x1c8 but only as blind reads.

## 3. Sequences

### 3.1 MAC reset + `_prepareHW` (0x806b8d78) [SEEN]
1. Clear `this[0xbc]` bit 0x200 (hw-powered).
2. Provider (`this[0xa8]`) vtable 0x354(1,0) then 0x358(1,0,0) (0x806b8dae, 0x806b8dc0). These are presumably clock-gate/power enables [INFERRED, vtable names unknown].
3. Look up platform function `function-device_reset` on the provider and call it with arg 1 (0x806b8dc8..de6). Only if the function exists.
4. `0x40 = 0x80000000`; read `0x40`.
5. PHY nub `powerUp(0)` via 0x806b8034 (calls PhyController vtable 0x344). Logs "POWER UP ... SUCCEEDED/FAILED".
6. `0x00 = 1`; `0x00 = 0`; `0x00 = 0x410` (0x806b8e76..0x806b8eb0).
7. `0x100 = 0x18`; read `0x100`; `0x100 = 0`; `0x1c4 = 0x80`; `0x1c0 = 0x1f8`; `0x104 = 8`; `0x108 = 0x18`.
8. `getHardwareAddress(NULL)`, then write MAC (3.4).
9. `0x118 = 1`; `0x10c = max_rx+4`; `0x110 = 0x600`; `0x114 = 0x600`.
10. `0x1c = TX ring phys`; `0x20 = RX ring phys`.
11. Read provider clock freq (provider vtable 0x348, 64-bit). If non-zero: `ticks = freq/2000`; `0x2c = 0x80000000 | ticks<<7`; `0x10 = ticks`.

### 3.2 Enable / link-up (`enable` 0x806b8c24, `_linkChange` 0x806ba2f8) [SEEN]
- `enable` calls `_prepareHW` if not prepared (flag 0x10 clear), then `_enableBSD` (0x806b9264) which fills the RX ring (0x806bb2c4) and starts the 1000 ms timer.
- On PHY link-up (`_linkChange(1)`): read link speed from PHY nub (0x806b80dc); program 0x100 = {0,4,1,5}; IOSleep(100 ms) (0x806ba56c); `0x108 |= 1`; `0x104 |= 1`; `0x04 |= 3`; `0x0c = 0x1f1`.
- On link-down: `0x04 &= ~3`; `0x0c = 0`; ack `0x08`; `0x108 &= ~1`; `0x104 &= ~1`.
- Disable (0x806b9328 `_disableBSD`, 0x806b9fd8 `_stopHW`): free rings, PHY powerDown via 0x806b8088, set `this[0xbc] |= 0x200`, provider vtable 0x358(0) and 0x354(0) (reverse of 3.1 step 2).

### 3.3 Descriptor rings

Both rings: 128 entries, **32-byte stride**, 0x1000-byte coherent buffer (IOBufferMemoryDescriptor, options 0x113, size 0x1000, alignment arg 1) [SEEN @0x806baf4e..0x806baf7a and 0x806bb226..0x806bb252]. Physical address from `getPhysicalAddress` (0x806bdbfc). Index wrap at 0x7f -> 0 (`cmp rN,#0x7e; ... movgt 0`) [SEEN @0x806bb1d8, 0x806bb454].

Descriptor layout (hardware words at +0..+0xc; +0x10/+0x14 are driver-only):

| off | TX | RX |
|---|---|---|
| +0x00 | status; **bit31 = OWN** (1 = DMA owns). Driver sets 0x80000000 last-in-loop per descriptor (0x806bb13e). Cleanup tests `ldr; cmp #0; blt` (0x806bb1ae..b2) | bit31 OWN; set 0x80000000 on refill (0x806bb37a). Done frame: bits[14:0] = length incl. 4-byte CRC (`bfc r3,#15,#17` @0x806bb3f4, `len-4` @0x806bb41c); **bit14 (0x4000) = frame error** (0x806bb3f0) |
| +0x04 | control: bits[?:0] = segment length; bit29 (0x20000000) = first segment; bit31 and bit30 (0xc0000000) set on last segment (interrupt/last-seg, exact names unknown); bit25 (0x02000000) = chained-next, preset for all 128 entries in `_resetTxRing` (0x806bafc8, 0x806bb000) and preserved | length of buffer (written from cursor segment length); **bit26 (0x04000000) = end-of-ring** on index 127 only (0x806bb370) |
| +0x08 | buffer phys address (0x806bb120) | buffer phys address (0x806bb364) |
| +0x0c | **next descriptor phys** (chain): base+idx*0x20+0x20, entry 127 points at ring base (0x806bb004..014) | not written (RX is contiguous, wraps via bit26) |
| +0x10 | driver: mbuf pointer on last segment (0x806bb138) | driver: mbuf pointer (0x806bb2e4 region) |
| +0x14 | driver: segment count on last segment (0x806bb13c) | unused |

TX max 8 segments per frame (`ETH_TX_MAX_SEGMENTS`, cursor `0x806bad44`, check `cmp r4,#8`), each segment up to 0xffff (cursor max seg size @0x806bad04). Free-slot check: `128 - this[0x10c] >= nseg` (0x806bb0e0).
The descriptor write order in the loop sets OWN on the first segment before later segments are filled (0x806bb13e inside the loop). For a Linux driver set OWN on the first descriptor last.

KDP polled mode (0x806bb5b4/0x806bb518) uses a single descriptor per direction in the same 4 KiB buffer: RX buffer at phys base, TX buffer at +0x800 (0x806bb7d0). RX desc: `+4 = 0x04000800` (size 0x800 + end-of-ring bit26), `+8 = buf`, `+0 = 0xe0000000` (0x806bb6a4..a8). TX desc: `+4 = len | 0xe4000000` (`bfi r5,#0xe4000,#12,#20` @0x806bb55e: bits31,30,29,26), `+8 = buf`, `+0xc = 0`, `+0 = 0x80000000`, then `writeReg(0x14,1)`, poll status bit0, ack (0x806bb54a..0x806bb58e). This is a useful minimal proof that a **1-entry ring with bit26 set works** and that **OWN is 0x80000000**.

RX buffers: mbuf of `max_rx+6` (0x806bb328), `mbuf_setdata(data+2, len-2)`-style 2-byte IP-align adjustment (0x806bb33c..0x806bb350) [INFERRED], then mapped with a 1-segment cursor (`0x806bae74`).

### 3.4 MAC address
`getHardwareAddress` (0x806b93b4): reads DT property `local-mac-address` (0x806b93d8) into `this+0xc8`. If all zero, falls back to a hardcoded bogus address `00:03:93:5b:...` (`c8=?, c9=3, ca=0x93, cb=0x5b, cc=0x9c, cd=1`, 0x806b944c..0x806b946a) with warning string "using bogus hardcoded station address".
No `setHardwareAddress` override (vtable slot 0x806c0e7c points outside this kext, inherited default). Programmed only in `_prepareHW` (0x806b8fa4..0x806b901e):
`0x120 = b0 | b1<<8`, `0x124 = b2 | b3<<8`, `0x128 = b4 | b5<<8` [SEEN].

### 3.5 TX kick
`outputPacket` (0x806bb02c, vtable 0x806c0e64) -> `_txQueue` 0x806bb0b4: build descriptors from 0x806bad8c (mbuf -> up to 8 segments), update `this[0x104]` (produce idx), `this[0x10c] += n`, then **`writeReg(0x14, 1)`** (0x806bb166..0x806bb16a) [SEEN]. If the ring is full the mbuf is parked in `this[0x110]` and retried from TX-done.

### 3.6 Interrupt (handler 0x806b872c, event source index 0 on provider, string 0x806be120)
```
status = R(0x08); W(0x08, status)                       // ack-all, W1C [0x806b8756..0x806b8774]
if status & 0x10:  _rxDone      (0x806bb3c8)            // RX complete
if status & 0x01:  _txDone      (0x806bb190)            // TX complete
if status & 0x20:  W(0x18, 1)                           // RX poll demand, likely "RX desc unavailable" [0x806b87a8]
if status & 0x100: v = R(0x1e0); W(0x1e0, v)            // stat-counter overflow ack [0x806b87ce..ec]
```
Mask value in use: `0x0c = 0x1f1` = bits {0,4,5,6,7,8}. Bits 6 and 7 are enabled but not handled by the ISR (names unknown).
KDP polled path: wait `R(0x08) & 0x10`, ack `W(0x08, 0x10)`.
`_rxDone`: loop while `desc.word0 & 0x80000000 == 0`; deliver `len-4`; refill via 0x806bb2e4.
`_txDone`: loop while in-flight > 0 and `desc.word0 & 0x80000000 == 0`; free mbuf at `+0x10`. TX error bits are never examined.

### 3.7 Multicast / promiscuous (vtable slots 0x806c0e80/e84/e88 = IOEthernetController `setMulticastMode`, `setMulticastList`, `setPromiscuousMode`) [INFERRED mapping, vtable order matches]
- `setMulticastList` (0x806b9794): per address CRC32 (poly **0x04c11db7**, LSB-first bit loop from the byte stream; constant at 0x806b98e8), `idx = crc >> 26` (6 bits), set bit `idx` in a 64-bit table; write `0x150 = lo[15:0]`, `0x154 = lo[31:16]`, `0x158 = hi[15:0]`, `0x15c = hi[31:16]`. Count 0 is an error. [SEEN; exact CRC bit order was only partially traced, so verify before relying on it.]
- `setMulticastMode(0)` (0x806b9710): if prepared, writes 0 to 0x150, 0x154, 0x158, 0x15c.
- `setPromiscuousMode` (0x806b9650): `0x118 |= 0x100` if arg==1, else `&= ~0x100`.

## 4. Interrupt status bits (0x08) seen in code

| bit | handler action |
|---|---|
| 0x001 | TX done (`_txDone`) |
| 0x010 | RX done (`_rxDone`) |
| 0x020 | write 0x18=1 (RX poll demand) |
| 0x100 | read/write-back 0x1e0 (stats) |
| 0x040, 0x080 | in the 0x1f1 mask, no handling seen |

## 5. MDIO (SMI)

`smiReadRegister` 0x806b9a38, `smiWriteRegister` 0x806b9c50 (logs "MDIO transaction timed out", string 0x806be92b). [SEEN]
- **0x1a0 command**: `phy_addr | (reg << 5)`; bits [4:0] PHY, [9:5] register. **bit10 (0x400) = read** (`orr r4,r0,#0x400` @0x806b9a54); write leaves it clear. **bit15 (0x8000) = start/busy**; software sets it with an RMW (`ldr; orr #0x8000; str`, 0x806b9ab2..0x806b9abc and 0x806b9cc4..0x806b9ccc) after the command was written.
- **0x1a4 data**: write path stores the 16-bit value here *before* setting the busy bit (0x806b9ca2); read path reads it after busy clears (0x806b9b62), low 16 bits kept (`strh`).
- Busy poll: `while (R(0x1a0) & 0x8000)` with a **100 ms** timeout (0x186a0 us, literal @0x806b9b78/0x806b9d7c). On timeout a read returns 0xffff.
- Read sequence: `W(0x1a0, phy | reg<<5 | 0x400); W(0x1a0, R(0x1a0) | 0x8000); poll; data = R(0x1a4) & 0xffff`.
- Write sequence: `W(0x1a0, phy | reg<<5); W(0x1a4, val); W(0x1a0, R(0x1a0) | 0x8000); poll`.
- PHY address comes from the DT property `address` of the PHY nub (`0x806bbce2`, stored `this[0x58]`), not hardcoded; you stated it is 0.
- Standard clause-22 MDIO. The 8742 MMD (clause 45-over-22) helpers `_smiMMDReadReg/_smiMMDWriteReg` exist (0x806bd1bc region, strings 0x806bfdba/0x806bfe11), used for WoL/EEE config. Not traced in detail.

## 6. PHY (AppleSMSC8742PhyController : AppleSMSC87XXPhyController)

PHY access goes through PhyController vtable +0x360 `read(reg,&val)` and +0x364 `write(reg,val)` [SEEN @0x806bbf54, 0x806bc44e]; these wrap the MAC smi helpers.
Registers used [SEEN]:
- **reg 0 (BMCR)**: reset = write 0x8000 and poll it clear (`reset` 0x806bc4cc @0x806bc502..).
- **reg 1 (BMSR)**: read twice; link = bit 2 (`ubfx r0,r0,#2,#1` @0x806bc122, `getLinkStatus` 0x806bc0ec).
- **reg 5 / reg 6**: partner abilities / autoneg expansion in `getLinkSpeed` (0x806bc15c @0x806bc27a, 0x806bc286 etc).
- **reg 0x12 (18, special modes)**: `setLinkSpeed` (0x806bc420) writes mode bits 7:5 = 0xe0 (auto/all), 0x00 (10H), 0x20 (10F), 0x40 (100H), 0x60 (100F) (0x806bc42e..0x806bc446).
- **reg 0x1e (30, interrupt mask)**: `_handleReset` writes **0x90** (0x806bc674..0x806bc67c) (bits 4 = autoneg-complete and 7 = link-down on the LAN8742 interrupt register layout [INFERRED from datasheet knowledge, not from this binary]).
- **reg 0x1f (31)**: read in `getLinkSpeed` (0x806bc182, 0x806bc212) to get the resolved speed/duplex [INFERRED role].
- PHY reset line: platform function `function-phy_reset` called on the PHY's provider, followed by `IOSleep(0x1f4 = 500 ms)` (0x806bc03e..0x806bc058) [SEEN], then PHY init state `(speed,5)`.
- PHY interrupt: `AppleSMSC8742PhyController::start` creates its own IOInterruptEventSource on its own provider, index 0 (string 0x806bfb5a). It is separate from the MAC interrupt. Which AIC line it is cannot be read from the binary (comes from DT).
- PHY regdump walks registers 0..31 (0x806bbf1c), nothing more.
- EDPD config (`_configureEDPD` 0x806bd068 region) and WoL (`_configureWoL` ~0x806bcc34) use MMD accesses, not traced.

## 7. DMA addressing, interrupt, clock

- **Physical addresses are programmed directly, no bus offset.** Ring base regs (0x1c, 0x20) take `IOMemoryDescriptor::getPhysicalAddress` (0x806bdbfc) results unmodified. Buffer addresses in descriptors (+0x08) come straight from `IOMemoryCursor::genPhysicalSegments`; nothing is added or masked in the stores at 0x806bb120 and 0x806bb364 [SEEN]. So bus address == CPU physical address (no IOMMU/DART in this path in the code).
- 32-bit addresses only (single 32-bit register for each ring base, one 32-bit word per buffer pointer).
- Descriptor stride 0x20 is used by the driver for both rings; TX follows `+0x0c` chain pointers explicitly (bit25 set). For RX no chain pointer is written, and the stride-0x20 contiguity is assumed by the hardware, so either the hardware has a descriptor-skip/stride field not seen in code, or RX stride is fixed at 32 [UNKNOWN which]. A Linux driver should use the same 32-byte stride.
- Interrupt: one source, `interruptEventSource(this, _handleInterrupt, _provider, 0)` = provider interrupt index 0 (string 0x806be120). That the line is AIC 27 is your input, not provable from the code.
- Clock: `ticks = provider_clock_hz / 2000` feeds 0x10 and 0x2c. The clock gate is controlled by provider calls (vtable 0x354/0x358), whose exact semantics are unknown here, but must be on before any register access.
- A timer (`_handleTimer`, 0x806b8808) runs every 1000 ms to refresh stats via `_updateStats` (0x806ba26c) from counters 0x1a8/0x1b4 idx 3,4 and 4..7; not required for operation.

## 8. Caveats

- Capstone linear sweep over Thumb data misreads literal pools as code; they are harmless noise in my listing. Function starts were found by `push {..lr}`.
- Role of which of 0x104/0x108 is TX or RX, 0x110/0x114/0x1c0/0x1c4/0x40 meaning, interrupt bits 0x40/0x80, and the RX descriptor stride mechanism are not determined from this binary alone. Probing the live hardware after enabling the clock is the way to settle them.
- The KDP RX length mask (`& 0x7ffb`, 0x806bb70c) looks odd (clears bit 2) and probably is an upstream bug or a `len-4` fold, so use the non-KDP `bits[14:0]` length.

## Verified on hardware (2026-10-22, Linux on the fsboot route, gate PMGR 0x3f101144 <- 0x20f)
MDIO via 0x1a0/0x1a4 exactly as above (cmd = reg<<5 | 0x400, then |0x8000, poll bit 15, data at 0x1a4), PHY address 0:
reg0 BMCR = 0x3100, reg1 BMSR = 0x782d (link up, autoneg complete), reg2/3 PHYID = 0x0007 / 0xc130 (SMSC LAN8742).
