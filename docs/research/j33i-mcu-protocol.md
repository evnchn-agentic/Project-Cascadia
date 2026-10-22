# J33i (Apple TV 3, AppleTV3,2) on-board MCU: UART protocol, IR events, status LED

Static analysis only. Nothing was run on the device, nothing was sent to the MCU.
Source build: 12H847 (iOS 8.4.2 / tvOS 7.2.x), the IPSW in `ipsw/`.

Confidence tags:
- **READ @addr**: seen directly in disassembly at that address.
- **GUESS**: inferred from context, not directly observed.

## 0. Summary

- The MCU is a **TI MSP430** (kext name `TiSerialFlasherIOCtrl`, firmware images `TiSerialFlasher-NN.NN.txt` in TI-TXT format, MSP430 code confirmed by disassembly).
  Besides IR it also does HDMI CEC / HPD (strings `cec init`, `hpd high`, `hpd low` in the firmware).
- **The protocol is not in the kernel.** Both kexts that match the `mcu0` node only wiggle GPIOs.
  The UART protocol lives in a user-space IOHID plugin:
  `/System/Library/Extensions/AppleTVIR.kext/Plugins/AppleTVIR.plugin/AppleTVIR` (rootfs, armv7 Mach-O bundle, 77088 bytes, has symbols and `__PRETTY_FUNCTION__` strings).
- Comm UART = uart1 (`mcu-comm0`) at **250000 baud, 8N1, no flow control**.
- Frame = `'S' 't' op seq lenHi lenLo body... crcHi crcLo`, CRC-16/CCITT-FALSE over everything before the CRC.
- Host commands: `0x02` version, `0x04` LED mode, `0x05` pairing, `0x07` system state. Replies `0x80` ACK and `0x81..0x86, 0x9f` errors.
- MCU events: `0x01` reset, `0x10` Apple IR (4 bytes), `0x11` IR repeat, `0x12` third-party IR timings, `0x05` pair event, `0xff` debug text.
- GPIO 2 (`mcu0` interrupt) is an **MCU-to-AP NMI request** that makes iOS enter the kernel debugger. IR does not need it.
- MSP430 bootloader (BSL) = uart0 + GPIO 0 (`mcu_rst`) + GPIO 1 (`mcu_tck`). Do not touch any of them.

## 1. Where the code is

| component | where | role | evidence |
|---|---|---|---|
| `com.apple.driver.TiSerialFlasherIOCtrl` | kernelcache, load 0x804eb000..0x804ed000 (+LINKEDIT to 0x804ef000), `__text` 0x804eb45c..0x804ebcd0 | two personalities: `TiSerialFlasherIOCtrl` (IONameMatch `mcu`) and `AppleMCUNMI` (IONameMatch `mcu-nmi`) | READ: `__PRELINK_INFO` plist, `_PrelinkExecutableLoadAddr` 0x804eb000 |
| `com.apple.driver.AppleTVIR` | kernelcache, load 0x80b4b000..0x80b4e000, `__text` 0x80b4ba30..0x80b4bed8 (0x4a8 bytes) | IOService shell (IONameMatch `mcu`, `HIDServiceSupport`), hosts the CFPlugIn below, root-only user client | READ: plist; strings 0x80b4bfb8.. |
| `AppleTVIR.plugin/AppleTVIR` | rootfs | **the whole protocol**: `MCUClient_*`, `_IRServer_*`, `IRDecoder`, `IRRemoteManager` | READ: symbols `_IOHIDPlugInFactory` 0xa48, `_IOHIDPlugInUnload` 0xc70; factory UUID 0516B563-B15B-11DA-96EB-0014519758EF |
| `/usr/bin/TiSerialFlasher` + `libTiSerialFlasher.dylib` | rootfs (dylib is in the shared cache, not extracted) | MSP430 BSL reflasher, `-f <firmware>`, `-d <device>` | READ: strings |
| `/usr/standalone/firmware/TiSerialFlasher-{01.32,02.07,03.05..03.07,04.01..04.04,05.01..05.03}.txt` | rootfs | MCU firmware images, TI-TXT, load 0xC000..0xFFFF | READ |

Plugin addresses below are plugin file offsets (bundle loads with `__TEXT` at 0). Firmware addresses are MSP430 addresses in image 05.03.
Which firmware version this unit actually runs is unknown; command `0x02` returns it (05.03 has the string `05.03` at 0xC107).

Extraction recipe (rootfs key 12H847 `048-37456-047.dmg` is the one used for BlueTool; see STATUS.md): `cascadia/scripts/rootfs-extract.py`. The extracted Apple binaries were kept in the session scratchpad, not in the repo.

## 2. UART settings

| setting | value | evidence |
|---|---|---|
| device | IORegistry `mcu0` property `uart-comm` (= `mcu-comm0`) -> `/dev/cu.mcu-comm0`; fallback `/dev/cu.builtin-serial4` (`uart-bsl` fallback `/dev/cu.builtin-serial6`) | READ @0x805c (`IOServiceNameMatching("mcu0")`), @0x80ec (`/dev/cu.%.*s`), @0x815a..0x81b0 |
| open flags | `O_RDWR \| O_NONBLOCK \| O_NOCTTY` (0x20006), then `TIOCEXCL` (0x2000740d), `fcntl(F_SETFL, \|O_NONBLOCK)` | READ @0x8228, @0x824e, @0x8274..0x8280 |
| iflag | clears `ICRNL\|IXON\|IXOFF\|IXANY` (`bic 0xf00`), so **no XON/XOFF** | READ @0x82e4 |
| oflag | clears `OPOST\|ONLCR` | READ @0x82ec |
| lflag | clears `ECHOE\|ECHO\|ISIG\|ICANON` (`bic 0x18a`) | READ @0x82f8 |
| cflag | bits 8..17 set to 0xcb = `CS8\|CREAD\|HUPCL\|CLOCAL`; `CSTOPB`, `PARENB`, `CCTS_OFLOW`, `CRTS_IFLOW` all 0. **8N1, no RTS/CTS.** | READ @0x82fe..0x8304 (`bfi r0, 0xcb, #8, #10`) |
| VMIN/VTIME | 0 / 0 | READ @0x830c..0x8312 |
| **baud** | `ioctl(fd, IOSSIOSPEED (0x80045402), 250000)` | READ @0x833e..0x8350 (`0x3d090`) |
| ADT | uart1 has no `no-flow-control` property but there are no RTS/CTS functions either (only `function-tx` = GPIO 7) | READ: ioreg-devicetree.txt line 490 |

Linux: the cascadia DT gives the Samsung UARTs a 24 MHz clock, and 24 MHz / 16 / 250000 = 6.000, so the divisor is exact.
`function-tx` for uart1 is GPIO 7 (port 0 pin 7, flags 0x102), so uart1 TX is pin 7. RX is probably pin 8, by analogy with uart2 where TX 18 / RX 19. That is a GUESS.

## 3. Framing

```
offset  0    1    2    3    4      5      6 .. 6+n-1   6+n    7+n
        'S'  't'  op   seq  lenHi  lenLo  body[n]      crcHi  crcLo
        0x53 0x74
```

| field | detail | evidence |
|---|---|---|
| sync | `0x53 0x74` ("St"). Receiver resyncs byte by byte on mismatch, logging `### resync: got 0x%02x ... instead of 'S'` / `'t'` | READ host TX @0x86de/0x86e4; host RX @0x8a60, @0x8a7e; FW RX @0xD1F8, 0xD210; FW TX @0xD6A6, 0xD6AC |
| op | 1 byte | READ @0x86ea; FW @0xC7B8 |
| seq | 1 byte. Host counter starts at **1 and adds 2** (odd numbers). The MCU echoes the request's seq in its reply. MCU-originated events use a separate MCU counter that also adds 2. | READ @0x800a/0x803c (init 1), @0x86f0..0x86f8 (`+2`); FW reply uses request seq @0xC7BC/0xC84A; FW event seq `incd.b &1899` @0xEAB4, 0xEC84 |
| length | 16-bit **big-endian** body length. Host accepts up to 0x400. MCU accepts at most 12 (`len+6 < 19`), else replies `0x85`. | READ @0x86fa..0x8702; host RX @0x8a96..0x8aa4; FW @0xD230..0xD252 |
| CRC | **CRC-16/CCITT-FALSE**: poly 0x1021, init 0xFFFF, MSB-first, no reflection, no final XOR. Covers header (6 bytes) + body. Sent **big-endian**. | READ host: CoreUtils `CRC16_CCITT(0xffff, hdr, 6)` then over body @0x871c..0x873c, hi byte at +6 @0x8736; FW CRC loop @0xE3A0 (`mov #-1,r15` ... `xor #4129`, MSB test 0x8000) |

Bad CRC on host RX returns -6713, on MCU RX the MCU replies `0x83` (READ FW @0xC7F8..0xC800).
Check value of this CRC on "123456789" is 0x29B1 (standard CCITT-FALSE).

Example frames, computed with the algorithm above (not captured):

| meaning | bytes |
|---|---|
| GetVersion, seq 1 | `53 74 02 01 00 00 fe bc` |
| LED mode 1 ("on"), seq 3 | `53 74 04 03 00 01 01 b1 2c` |
| LED mode 0 ("off"), seq 3 | `53 74 04 03 00 01 00 a1 0d` |
| system state 0 ("booted"), seq 5 | `53 74 07 05 00 01 00 68 46` |

### Transaction rules (host side)

| rule | value | evidence |
|---|---|---|
| per-byte read/write timeout | 3000 ms (`select()` on the fd plus a cancel pipe) | READ @0x13a4 (`0xbb8` -> ctx+0x420), use @0x8dea, @0x8e52, select @0x8fac |
| attempts | 5 (write, then wait for response; retry on any error, logged `Retrying opcode`) | READ @0x8488..0x850c |
| response wait | keeps reading packets for up to 30 s; packets that are not a response go to the event handler; then `MCU LIVELOCK DETECTED` | READ @0x87da..0x87f8, @0x8926 |
| response = | any op in 0x80..0x86 or 0x9f (bitmask 0x8000007f on op-0x80) | READ @0x8846..0x88fe |
| success | `0x80`, or `0x84` when the request was op 7 | READ @0x8900..0x8910 |
| events | MCU pushes events unsolicited; host never ACKs them | READ thread loop @0x3bc2..0x3c06 (ReadPacket then `_IRServer_ProcessMCUEvent`, no write) |
| threading | all commands are queued through a pipe and run on the one IR thread, so reads and transactions never interleave | READ @0x3b60..0x3c3a |

### Reply / error opcodes

| op | host maps to | MCU sends it when | evidence |
|---|---|---|---|
| 0x80 | success (body = reply data) | OK | READ host @0x890c; FW @0xC806 |
| 0x81 | -6711 | unknown opcode | READ host @0x89a0; FW @0xCA04 |
| 0x82 | -6735 | opcode 0x06 / 0x29 (known but refused) | READ host @0x89b0; FW @0xCA22 |
| 0x83 | -6713 (checksum) | bad CRC | READ host @0x89cc; FW @0xC800 |
| 0x84 | -6745, except success for op 7 | GUESS: "already in that state" | READ host @0x8900 |
| 0x85 | -6743 (size) | body too long / wrong length | READ host @0x89e0; FW @0xCA18, 0xD252 |
| 0x86 | -6750 | GUESS: body too short | READ host @0x89f0; FW @0xCA0E |
| 0x9f | -6736 | value out of range (e.g. LED mode >= 7) | READ host @0x8a04; FW @0xC868 |

Error-code names are GUESSes from the CoreUtils error table; the numbers are READ.

## 4. Inbound events (MCU -> AP)

Dispatcher `_IRServer_ProcessMCUEvent` @0x249c.

| op | name (log string) | body | host action | evidence |
|---|---|---|---|---|
| 0x00 | `MCU NoOp` | any | log | READ @0x2622 |
| 0x01 | `MCU PowerOnReset` | any | re-run `_IRServer_InitMCU` (0x3fb4) | READ @0x24d8..0x2534; FW sends it at boot @0xD99C |
| 0x05 | `MCU Pair Event` | any | log only | READ @0x2660 |
| 0x10 | Apple IR command | exactly 4 bytes | decode, post HID | READ @0x26b0 (`cmp r5,#4`) |
| 0x11 | `Apple IR Repeat` | none | repeat the last button (dropped if there was no recent press) | READ @0x25a0..0x261c |
| 0x12 | third-party IR | `u16le mcu_time_us`, then `u16le` pulse/space timings, at most 0x201 bytes | learned-remote decoder (NEC, Sony SIRC, RC-5, RC-6, Samsung, ...) | READ @0x286a..0x2980 |
| 0xff | `MCU Debug: %.*s` | ASCII text | log (trailing NUL/LF stripped) | READ @0x253a..0x2594 |
| 0x21, 0x22, 0x29 | not handled (`### MCU Unexpected Opcode`) | | log | FW sends them @0xC452, 0xE1AE, 0xDC94; meaning GUESS: CEC/HPD related |

There is **no button-release event**. The host synthesises key-up on a timeout (`_IRServer_ProcessHIDTimeout`). A 250 ms tick value is stored at plugin init (READ @0xaf0); that it is the release timeout is a GUESS.

### Apple IR body (op 0x10)

With body bytes `b0 b1 b2 b3`, the host computes (READ @0x26b6..0x27ee):

| field | formula | meaning |
|---|---|---|
| vendor | `((b0<<4) \| (b1>>4)) & 0xffe` | logged only, never checked (READ: no compare in the path) |
| page | `b1 & 0x1f` | command page |
| device | `b2` | remote's pairing UID |
| cmd | `b3 >> 1` | command; bit 0 of b3 is the parity bit |
| parity | `Parity32(b0<<24\|b1<<16\|b2<<8\|b3)`; bad parity is **logged but not rejected** | READ @0x2774, @0x27ba |
| key | `page<<8 \| cmd` | switch value |

GUESS: on the wire this is Apple's NEC frame, address 0x87EE, so b0=0x87, b1=0xEE, page 0x0E = the classic white-remote command set, page 0x05 = the aluminium remote's extra commands.
The legacy cmd values (menu 1, play 2, right 3, left 4, up 5, down 6) match the Apple remote codes known from other drivers. That is corroboration, not a reading.

If the box is paired, commands whose `device` differs from the paired UID are dropped (`Ignoring unpaired remote command`) (READ @0x3818..0x3826).

### Key table

Resolved from jump tables @0x2804 (keys 1..7), @0x2b4c (0x501..0x519), @0x2be6 (0xe01..0xe0c), @0x2c86 (0xe2e..0xe35).
"GD" = HID Generic Desktop page 0x01, "C" = Consumer page 0x0C.

| key | log string | HID posted | suggested Linux key | evidence |
|---|---|---|---|---|
| 0xe05, 0x501 | BUTTON: Up | GD 0x8C (System Menu Up) | KEY_UP | READ @0x2f9e |
| 0xe06, 0x502 | BUTTON: Down | GD 0x8D | KEY_DOWN | READ @0x2ff2 |
| 0xe04, 0x503 | BUTTON: Left | GD 0x8B | KEY_LEFT | READ @0x2f4a |
| 0xe03, 0x504 | BUTTON: Right | GD 0x8A | KEY_RIGHT | READ @0x2ef6 |
| 0xe01 | BUTTON: Menu | GD 0x86 (System App Menu) | KEY_MENU / KEY_BACK | READ @0x2c2a, 0x380e |
| 0x505, 0xe2e | BUTTON: Select | C 0x41 (Menu Pick) | KEY_ENTER / KEY_OK | READ @0x2cf2 |
| 0xe02 | BUTTON: Play/Pause/Select | GD 0x89 (System Menu Select). Ignored if this remote UID has already sent an aluminium-remote code (`Ignoring A39 play/pause/select from B39`) | KEY_ENTER (white remote centre button) | READ @0x350e..0x37e8 |
| 0xe2f | BUTTON: Play/Pause | C 0xCD | KEY_PLAYPAUSE | READ @0x3044 |
| 0x50b | BUTTON: Play | C 0xB0 | KEY_PLAY | READ @0x2bce |
| 0x519 | BUTTON: Pause | C 0xB1 | KEY_PAUSE | READ @0x34f8 |
| 0x518 | BUTTON: Stop | C 0xB7 | KEY_STOP | READ @0x34a8 |
| 0x50d / 0x50e | FastForward / Rewind | C 0xB3 / 0xB4 | KEY_FASTFORWARD / KEY_REWIND | READ (table) |
| 0x511 / 0x512 | Next / Previous | C 0xB5 / 0xB6 | KEY_NEXTSONG / KEY_PREVIOUSSONG | READ (table) |
| 0x513 / 0x517 | Replay10Seconds / SkipAhead | C 0xC7 / 0xC6 | optional | READ @0x3408, 0x3458 |
| 0x001 / 0x002 | BUTTON: Pair / Unpair (Menu+Right / Menu+Left hold on the remote, GUESS) | op 5 to the MCU + prefs | handle in driver or ignore | READ @0x2d40..0x2e5c |
| 0x003, 0x007 | Low battery | page 0x85 usage 0x4B | KEY_BATTERY / log | READ @0x2862 |
| 0xe0b | Menu+Up (black-screen recover) | internal | ignore | READ table |
| 0xe07, 0xe32 / 0xe08, 0xe33 / 0xe35 | Play+Up screenshot / Play+Down stackshot / Play+Right send logs | internal | ignore | READ table |
| 0xe0c | SystemReset (unsupported) | nothing | ignore | READ @0x3662 |

Note on the "consumer 0x40..0x45" observation: the IR path never posts C 0x40, 0x42..0x45. It posts GD 0x86/0x89..0x8D plus C 0x41 (READ above).
The consumer 0x40..0x45 usages must be another accepted input path in backboardd (the `atvkey` injector). There is no conflict, and a Linux driver should just emit KEY_* codes.

## 5. Outbound commands (AP -> MCU)

| op | body | meaning | used when | evidence |
|---|---|---|---|---|
| 0x02 | none | **get version**; reply 0x80 body = ASCII `"NN.NN"` (+NUL, GUESS), parsed `%d.%d` | `_IRServer_InitMCU` | READ host @0x851c, @0x402e, @0x4048; FW @0xC83A |
| 0x04 | 1 byte LED mode | **status LED ("SIL", status indicator light)** | init, `SILControl`, `systemEvent`, after each key release | READ host @0x85c8; FW @0xC850 |
| 0x05 | `[paired(0/1), deviceUID]`; FW accepts `[0]` alone = unpair | pairing filter | init, `pairControl`, Pair/Unpair buttons | READ host @0x8568, @0x416c, @0x2200; FW @0xC86E |
| 0x07 | 1 byte: 0 = booted, 1 = standby, 2 = normal (wake) | system state | display power on/off, system power callbacks | READ host @0x85a8, @0x1f46..0x1f50, @0x1d3c, @0x3fa4/0x3fac; FW @0xC896 |
| any | raw | `com.apple.AppleTVHID.sendRawMCUCommand` passes first byte = op, rest = body | debug property | READ @0x18f0..0x18fc |

### LED modes (op 0x04)

Host string to value parser @0x85e8 (READ). A decimal number 0..255 is also accepted and passed through.

| value | host name | host uses it for | FW 05.03 behaviour (handler 0xCBA2, table 0xCBD8) |
|---|---|---|---|
| 0 | `off` | `systemEvent off` | solid off (READ) |
| 1 | `on` | `starting`; default after each key release when no default is set | solid on (READ) |
| 2 | `slow` | `hardwareProblem` (also becomes the new default) | blink, 500 tick period (READ @0xCC48) |
| 3 | `fast` | | blink, 100 tick period (READ @0xCC60) |
| 4 | `1blink` | | one blink, 700 ticks (READ @0xCC78) |
| 5 | `3blinks` | `rejectedCommand` | **table entry is the same `clr.b r12` stub as mode 7, so the MCU answers 0x9F** (READ 05.03 @0xCCE2; same in 04.04, whose table is at 0xCE0E with mode 5 -> 0xCF18 = mode-7 stub). So iOS's `rejectedCommand` blink is probably a silent no-op on this hardware. |
| 6 | (no host name) | | blink pattern, 500 ticks x 39 (READ @0xCCBE). Meaning unknown. |
| >= 7 | | | rejected 0x9F (READ @0xC856) |

`SILControl "morse:<wpm>:<text>"` makes the host bit-bang modes 1/0 through CoreUtils `MorseCode` (READ @0x1e2a..0x1e96, callback @0x2414).
The init sequence sends op 4 only if a SIL default is set (READ @0x409a..0x40f0).
The MCU blinks the LED by itself when it receives IR: the 0x10 send path first calls the LED routine with internal mode 9 (READ FW @0xCEFE..0xCF02).
That is why the host re-sends its default LED mode after every key release (READ host @0x42cc..0x4332, sends op 4 with the default or 1).

**White vs amber: not found.** The protocol carries a single mode byte, with no colour field anywhere in host or firmware dispatch.
The firmware's LED routines are generic GPIO set/clear/toggle helpers (0xDDE8, 0xE0DA, 0xE31A) indexed by a pin number, and I did not resolve which pins they drive. GUESS: J33i has one white LED, and "amber" is an Apple TV 1st-gen memory.

### Init sequence the iOS host runs (`_IRServer_InitMCU` @0x3fb4)

1. `0x02` get version (READ @0x402e)
2. `0x07` system state 0 "booted" (via `_IRServer_PowerOnDisplay(ctx, false)` @0x4096 -> 0x1ecc)
3. `0x04` LED default, if one is configured (READ @0x40f0)
4. `0x05` `[paired?1:0, pairedUID]` (READ @0x416c)

It runs again on every MCU `0x01` PowerOnReset event.
There is no handshake or wake-up before the first command: the host just opens the port and sends op 2.

## 6. GPIOs on the `mcu0` node

From ioreg (`function-*` = phandle 0x0e = `gpio@FA00000`, FourCC `GPIO`, pin, flags):

| function | SoC GPIO | user | meaning | evidence |
|---|---|---|---|---|
| `function-mcu_rst` | 0 | TiSerialFlasherIOCtrl / AppleTVIR kexts | MSP430 RST/NMI pin | READ ioreg line 666; kext 0x804eb61c drives signal 0 = `[+0x50]`, 1 = `[+0x54]` via platform function, root-only user client `extWiggle` |
| `function-mcu_tck` | 1 | same | MSP430 TEST/TCK pin. The RST+TEST pattern is the **BSL entry** sequence | READ ioreg line 670 |
| interrupt `<2 2>` / `function-sos` (flags 0x100) | 2 | AppleMCUNMI | **MCU asserts it to request an NMI**: the interrupt handler calls `PE_enter_debugger("MCU NMI")`. The KDP callout drives `function-sos` while the AP sits in the debugger | READ handler 0x804ebc20 (`b.w PE_enter_debugger`); start 0x804ebaf8..0x804ebb60; callout 0x804ebbc4. Callout semantics are a GUESS. |

GPIO 2 is **not** an IR or wake interrupt: IR arrives only as UART frames. On Linux leave it an input. Optionally request it as an IRQ and just log edges.
What makes the MCU assert it (GUESS: a held remote combo) was not traced.

## 7. DO-NOT-SEND / DO-NOT-TOUCH

| item | why |
|---|---|
| **GPIO 0 (`mcu_rst`) and GPIO 1 (`mcu_tck`)**: never configure as outputs or toggle | the RST/TEST sequence puts the MSP430 into its ROM bootloader (BSL). |
| **uart0 (`mcu-bsl0`, 0x2500000)**: keep it `status = "disabled"` in the Linux DT and send nothing on it | it is the BSL link (READ: ADT name `mcu-bsl0`, plugin fallback `uart-bsl`). GUESS from general MSP430 knowledge, not from this code: on MSP430 ROM BSLs, a wrong BSL password triggers a **mass erase of main flash**, leaving the MCU without firmware and the box without IR, LED or CEC until it is reflashed. |
| `TiSerialFlasher`, `-f`, any TI-TXT image | reflashes the MCU. |
| uart1 `function-tx` as GPIO 7 | iOS has a GPIO function on the TX pin. Use the pin only as UART TX. |
| ops other than 0x02, 0x04, 0x05, 0x07 | the firmware also implements 0x06/0x29 (refused), 0x08 (1 byte, 0/1), 0x09 (1..3 bytes, reads flash bytes at 0xC0DF..0xC0E2), 0x0A (needs 0xFD, returns 14 bytes of RAM counters), 0x20 (subcommands 0,1,2,0x10,0x11,0x20), 0x28 (even-length u16 array). None has a host caller in 8.4.2 and none was traced. The only flash-programming code found (0xD2B6, writes FCTL at 0xD2E2..0xD328) is called from MCU boot (0xD95E), not from a command, but that check was not exhaustive. |
| raw op 0x07 values other than 0, 1, 2 | the MCU rejects them. Value 1 (standby) is harmless but turns off HDMI-side behaviour (GUESS). |

## 8. Smallest safe Linux experiment

**Step 1: listen only, transmit nothing.**

1. Enable uart1 (`serial@2600000`, irq 22, same `apple,s5l-uart` binding and 24 MHz clocks as uart2).
   Do not add or change pinctrl until the current GPIO 7/8 config registers have been read (read-only).
   iBoot/iOS probably left them muxed to the UART.
2. Leave uart0 disabled, and leave GPIO 0/1/2 unclaimed.
3. `stty -F /dev/ttySAC1 250000 cs8 -cstopb -parenb -crtscts -ixon -ixoff raw -echo`, or pyserial with `baudrate=250000`, then hexdump.
4. Press Up on the Apple remote. Expected: `53 74 10 ss 00 04 b0 b1 b2 b3 cH cL`, then `53 74 11 ss 00 00 cH cL` repeats while held. `ss` even (GUESS).
   Check the CRC offline with CRC-16/CCITT-FALSE over the 10 bytes before it.
5. A cold MCU reset may also show `53 74 01 ...` (PowerOnReset) and `53 74 ff ...` debug text.

Listening alone should be enough. The system-state byte that op 7 writes (`&1900`, FW @0xC8A4/0xC8AE/0xC8B8) is never read anywhere else in 05.03 (READ: no other reference in the disassembly), so IR sending is not gated on op 7.
Caveat: the disassembly is linear and does not follow indirect accesses. Other gates (`&1896`, `&1894` on the 0x10 path, @0xCF04) were not traced.
If nothing arrives, suspect pinmux or baud first, then go to step 2.

**Step 2: one read-only query.** Send GetVersion, `53 74 02 01 00 00 fe bc`. Expect `53 74 80 01 00 0N <"NN.NN"> cH cL`.
That proves TX framing, CRC and baud in one shot. Only after that, op 7 = 0 ("booted"), then op 4 to drive the LED.
