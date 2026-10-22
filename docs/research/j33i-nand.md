# j33i (Apple TV 3, AppleTV3,2) NAND: what it is and how Linux can use it

Read-only, offline research (2026-10-22). No device was touched. Every claim is
tagged READ (seen in a file on this Mac) or GUESS (inference or memory,
not verified here).

Sources read: `work/ioreg-devicetree.txt` and `work/ioreg-service.txt` (a live
IORegistry dump of this Apple TV, so real numbers), `work/j33i-full.dts`,
`work/kc.j33i.macho`, `work/iBEC.j33i.bin`, `work/iBoot.j33i.bin`, `STATUS.md`,
and the existing iPad mini notes `docs/research/p105-nand.md` plus
`tools/nand/` and `patches/files/drivers/mtd/devices/mtd-apple-h2fmi.c`.

## Short answer

The Apple TV 3 has the same NAND stack as the iPad mini 1 that this project
already reads: Hynix PPN managed NAND behind FMI, with Apple's SWISS PPN FTL
(not yaFTL, not raw NAND). The iPad work (userspace `nandctl`/`iosnand`, an MTD
read driver) is a close fit; the Apple TV has one bus and two dies where the iPad
has two buses and four. Reading iOS is a small port. Writing safely is not
worth it. The recommended path never writes the NAND: root over Ethernet (NFS or
NBD), optionally with iOS's NAND as a read-only extra.

## 1. What the NAND stack is

| item | value | evidence |
|---|---|---|
| Interface | Apple FMI (`flash_controller0@1200000`, `fmi,s5l8940x` / `s5l8920x`), one bus populated | READ: the node has six 4 KB windows, two FMI + FMC + ECC sets exactly like the iPad (live `IODeviceMemory` 0x31200000/0x31240000/0x31280000 and 0x31300000/0x31340000/0x31380000), but only FMI0 CE0 has a chip (`#ce 1`, `ce-bitmap 1`, `#databus 1`). Corrected 2026-10-22: an earlier version of this row said one set |
| Media type | PPN ("physical page NAND"): the package has its own controller, host sends a command protocol over Toggle-DDR | READ: `ppn-device = Yes`, `ppn-spec-version 05050100` (v1.5.5), `device-readid` bytes decode to "PPN", iBEC/iBoot print "PPN Device Detected" |
| Vendor / part | Hynix, marketing name `20nm64Gb`, firmware `040472P_HYNIX_B_`, manufacturer id `AD 82 52 23 21 01` | READ: `controllers` property in ioreg-devicetree.txt |
| Geometry | 1 CE, 2 CAUs, 1064 blocks per CAU (2128 per CE), 256 pages per MLC block (128 SLC), page 16384 + 64 bytes, 4 KB logical pages with 16 bytes metadata each | READ: same property and `#ce-blocks 2128`, `#block-pages 256`, `logical-page-size 4096` |
| Raw size | 2 x 1064 x 256 x 16 KB = about 8.9 GB; iOS exposes `disk0` = 8,000,000,000 bytes | READ: ioreg-service.txt `Size = 8000000000` (raw figure is arithmetic) |
| Flash partition scheme | `IOFlashPartitionScheme`: Boot Block, Bad Block Table, NVRAM, Firmware, System Config, Effaceable, Diagnostic, Filesystem (the FTL) | READ |
| FTL | `AppleSwissPPNFTL` (class present in ioreg-service.txt), `ppn-ftl-ver = 2`. Not yaFTL | READ |
| Layers above | `LightweightVolumeManager` (LwVM) -> GPT-ish `disk0s1`, then `System` (disk0s1s1, 1,399,549,952 bytes) and `Data` (disk0s1s2, 6,527,684,608 bytes, content-protected) | READ for sizes and names; HFS+ on both is GUESS by analogy with iPad (the iPad notes verified HFSX on System) |
| Boot path | iBEC and iBoot carry the full read stack: `h2fmi`, `fmiss_ppn`, `WhimoryPPN` (`_ppnvflInit`, `_ppnfpartReadSpecialBlock`), `SVFL` / `SFTL` (`_findOnNANDCxt`, `_L2V_*`), HFS. This is how iBoot loads the kernelcache and our 32 MB `/linux.kc` | READ: strings in iBEC.j33i.bin and iBoot.j33i.bin |
| Who drives FMI in iOS | `AppleIOPFMI` (an IOP coprocessor, `s5l8942xiop_firmware`) drives FMI; the AP does not | READ: class names in ioreg; same conclusion as the iPad notes |

What the kernelcache adds: `ppnvfl*`, `ppnfpart*`, `sftl:` error strings
("lba mismatch", "program failure writing cxt ...", BTOC, cxt SB array), and
`AppleNANDFTL`. Because the kernelcache is shared across many devices, its
presence alone proves nothing; the ioreg and iBoot strings are what prove
this device is PPN + Swiss. The kernelcache also contains the legacy raw-NAND
messages (`FIL_VS_TOSHIBA_2D NAND not supported`), so a raw-NAND path exists
in the code but this unit uses PPN (READ).

Differences from the iPad mini 1 (P105) that matter for code reuse:

- 1 populated bus instead of 2, so 2 dies (CAUs) instead of 4. FMI0 is at
  0x31200000 (READ, confirmed 2026-10-22: live `IODeviceMemory`, and iBoot's
  own `h2fmi_init_minimal`; see "Evidence" below). FMI1 exists and iBoot
  clocks it, but nothing is behind it.
- 8 GB class (Data 6.5 GB) instead of 16 GB.
- The iPad's stripe/sequence arithmetic assumed 4 dies (`+16 per page`); with
  2 dies the per-page sequence step is probably +8 (GUESS; the LBA plus
  highest-sequence-wins rule does not depend on it).
- `ce-bitmap = 1` (only CE0). Same absent-CE wedge trap applies: never read
  a CE outside the bitmap.

## 2. Prior art

I cannot browse from here, so all of this is from memory (GUESS on details,
repo names I believe exist; check before relying on any).

Reading Apple NAND with an FTL reimplementation:

- openiBoot (iPhone Dev Team / later iDroid Project), e.g.
  `github.com/iDroid-Project/openiBoot`. Contains FMI drivers, VFL and
  yaFTL for S5L8900 (iPhone 2G/3G, iPod 1G/2G), S5L8720 (iPod touch 2G),
  S5L8920/8922 (iPhone 3GS, iPod 3G) and the A4 (S5L8930: iPhone 4, iPad 1,
  Apple TV 2). Raw NAND + yaFTL, not PPN. Useful as a register-level and
  ECC (BCH) reference for the FMI block, which is shared with this chip.
- iDroid Project Linux kernels (`iDroid-Project/linux`, plus `iphone-linux`
  by Ricky Taylor / planetbeing, iphonelinux.org): Android on those same
  S5L8900-A4 devices, with an MTD-ish FMI/VFL/FTL driver. Same limit: raw
  NAND era.
- `iphone-dataprotection` (dinosec / Jean-Baptiste Bedrune and Jean Sigwald,
  `github.com/dinosec/iphone-dataprotection`): Python tools (`nand.py`,
  `vfl.py`, `ftl.py`) that parse YAFTL/VFL dumps of S5L8920/8930 era NAND,
  and kernel-extension tooling to dump the NAND from a booted iOS. Reads
  dumps; no PPN.
- planetbeing/xpwn and the `ipsw` tooling cover image formats, not NAND.

PPN / Swiss FTL (A5 and later, which is this chip): I know of no public
open-source reader or Linux driver for SWISS/SFTL or the PPN protocol
that I can verify exists. The mainline kernel has no Apple FMI/PPN NAND
support (GUESS but confident). Asahi Linux's work is on the M-series NVMe-like
ANS, unrelated. Corellium's iPhone 7 Linux ("Project Sandcastle") is on a later
storage stack (GUESS).

So the useful prior art for this device is this repository itself: `tools/nand`
(`ppn.c`, `iosnand.c`, `nandctl.c`), `tools/nand-ftl-scan.py`, and the
`mtd-apple-h2fmi.c` read-only MTD driver, all derived from iBEC disassembly
and verified on the iPad (full dump, System partition rebuilt, `fsck_hfs`
clean, hundreds of binaries pass `codesign -v`; READ in p105-nand.md).

## 3. Options, ranked

Ranking is by effort first, then iOS risk. "Risk" means risk to the iOS install.

| # | Option | Effort | iOS risk | What you get |
|---|---|---|---|---|
| 1 | (d) Network root over Ethernet: NFS root or NBD/iSCSI | already working (Ethernet done, DHCP works per STATUS.md) | none | Unlimited read/write persistence on the host's disk. Needs a LAN host. |
| 2 | (c-lite) Larger initramfs bundle in the file iBoot already loads | small | none new (the 32 MB file is already there) | Read-only rootfs in RAM. Bounded by RAM (about 512 MB, DTS placeholder, GUESS) and by iBoot's loader (heap panic seen at 32 MB, STATUS.md). Nothing persists. |
| 3 | (d) USB storage on the micro-USB port | unknown, probably high | none | Port is wired as a device/service port: STATUS.md uses it as a CDC gadget. Whether it can be host is GUESS and unverified; do not count on it. |
| 4 | (a) Read-only iOS partitions via the existing PPN stack | medium: retarget `ppn.c`/`iosnand.c` to 1 bus x 2 dies, rerun the first-run scan, add `ios mount` | none if strictly read-only and only AP drives FMI | Linux can read iOS System (and, with the AES engine, nothing of Data). Also lets a prebuilt image file on the System partition be loop-mounted read-only from Linux. |
| 5 | (c) A loop/disk-image file on the iOS System partition written by iOS, read by Linux via (4) | medium on top of (4) | low (iOS writes through its own FTL, same as `/linux.kc` today) | A large read-only rootfs image bigger than RAM, with a RAM/tmpfs overlay. Writes do not persist. Free space on System is not known, GUESS small. |
| 6 | (e) Fixed 2-way sneakernet: Linux writes a state file to the network or over the CDC gadget; iOS is not involved | trivial | none | Cheap persistence for config only. |
| 7 | (b) Read/write FTL, or shrink `fsys` to give Linux its own flash partition | very high | high to total | A real Linux NAND. Not recommended. |

Why (b) is bad (READ for the structure, from p105-nand.md): the FTL is log
structured and its metadata is global across all dies. A write is not
confined to one partition: it allocates blocks, rewrites shared context pages
(type-2 / L2V spans), and does GC and wear levelling. A mistake corrupts the
map for the whole volume, iOS included. The write path lives in
`AppleSwissPPNFTL` in the kernelcache and has not been reverse engineered
(the `sftl: program failure writing cxt ...` strings in kc.j33i.macho confirm it
is there). Carving out a flash partition by editing the `ndrG` table
avoids the FTL but needs a restore, a bootloader-area write, and costs
over-provisioning (iPad numbers; the Apple TV's are untested). One
LwVM-level observation from the iPad that probably transfers: spare unmapped
LwVM chunks exist, but writes into them still go through the FTL, so they
buy nothing.

One extra risk specific to this device: iOS's IOP firmware owns FMI while
iOS runs (READ: `AppleIOPFMI`). Linux touching FMI from the AP while the IOP is
alive, as in a kloader-from-iOS boot, could collide with it. The iPad notes
found two processes on one bus wedge the FMC and leave the PPN answering
errors. The iBoot/fsboot path, where iOS is not running and the IOP is not
started, avoids this (GUESS that the IOP is idle after that path; verify
before a first NAND read).

## 4. Recommended path (never risks iOS)

1. Primary: network root. Use NFS or NBD over the Ethernet that already
   works (Arasan FE, DHCP verified). This is persistent, large, writable,
   needs no flash access and cannot harm iOS. Effort: about 1 day of
   polish, mostly rootfs/init and a host-side export. Same design as the
   iPad's NFS root.
2. Keep the 32 MB file loaded by iBoot as the kernel + initramfs that
   brings up the network, then pivots to the network root. This is what the
   current chain already does (READ in STATUS.md); no new NAND writes.
3. Optional extra, read-only: port the iPad PPN read stack for iOS files.
   - Retarget `tools/nand/ppn.h` constants (buses 1, CAUs 2, dies 2,
     blocks 1064) and the page-sequence step. Run `nandctl` read ID and
     PPN device parameters first and compare with the ioreg values above
     (`AD 82 52 23 21 01`, 16448 byte pages, `040472P_HYNIX_B_`).
   - Enforce the existing opcode allowlist (no program, no erase, no set
     feature). Keep the per-bus lock. Do it from the fsboot path, not from an
     iOS kloader boot, to avoid the IOP.
   - Run `iosnand` (page-table scan then NBD) and `ios mount`.
   - Effort: about 2 to 4 days to a mounted System partition, assuming
     the PPN command set is identical (it should be: same iBEC code and
     same Hynix family; GUESS). Add about 1 to 2 weeks for a proper
     DMA-based kernel MTD driver (the iPad has the PIO-only userspace one and
     a kernel read-only skeleton).
   - What it buys: reading iOS's System partition (about 1.4 GB). Not
     iOS's Data (per-file encryption and the AES engine). Mostly
     curiosity value for a TV box; no persistence for Linux.
4. Do not do (b). If Linux ever needs its own flash, plan it as a restore
   and re-flash project with iOS gone, not a coexistence.

Open items to check before any read of this NAND (none were verifiable
offline):

- ~~Confirm the FMI base address~~ Done 2026-10-22: 0x31200000, READ from the
  live IORegistry and from iBoot (see "Evidence").
- Confirm how much free space the System partition has, if option (5) is wanted.
- Confirm whether the micro-USB port can ever be host-mode on this board; no
  evidence for it in the files read.
- Confirm RAM size (DTS says 512 MiB as a placeholder; the real value is
  patched at boot, GUESS 512 MB).

## 5. The read-only port (2026-10-22, compile-only, not yet run)

Offline and compile-only: nothing below has touched the box. `BOARD=j33iap
./cascadia build` is green with no new warnings (the driver recompiled clean;
the NAND tools build clean with `-Wall -Wextra` for both boards; `dtc` warns
about nothing, before or after). The kernel node is `status = "disabled"`, so
the image boots exactly as before.

### Evidence: FMI base, gates, geometry

| item | J33i value | evidence |
|---|---|---|
| FMI0 / FMC0 / ECC0 | 0x31200000 / 0x31240000 / 0x31280000 | READ: live ioreg `IODeviceMemory` 824180736, 824442880, 824705024. READ: iBoot.fsboot (base 0x9FF00000) `h2fmi_init_minimal` = 0x9ff037fc: `0x9ff0383c ldr r0,[0x9ff03894]` (= 0x31300000), `cmp r5,0; itt eq; movs r0,0; movt r0,0x3120` at 0x9ff0383e-0x9ff03844, stored at `[fmi+4]`; so regs = bus ? 0x31300000 : 0x31200000. `0x9ff04ffc` (bus index) compares `[fmi+4]` against 0x31200000 (literal 0x9ff0500c) |
| FMI1 | 0x31300000, **clocked, empty** | both callers of `h2fmi_init_minimal` (0x9ff02eee, 0x9ff02efa) init both buses, and it turns the bus's gates on (`bl 0x9ff22580` with on = 1 at 0x9ff03858). Nothing is behind it: `ce-bitmap 1`, `#ce 1`. So "gates on" does not mean "safe to read" there |
| interrupts | 33 (FMI0), 34 (FMI1) | READ: ioreg `interrupts <21 22>`, ADT `<33 34>` |
| clock gates | FMI0 0x3f1010c4, FMI0-BCH 0x3f1010c8, FMI1 0x3f1010cc, FMI1-BCH 0x3f1010d0 | READ: ADT `clock-gates <59 61 60 62>`; device-clocks entries 59-62 (FMI0, FMI0-BCH, FMI1, FMI1-BCH) have power indices 49-52, i.e. 0x3f101000 + 4 x index; iBoot `WMR_CLOCK_GATE` 0x9ff22580 gates ids 0x2f/0x30 (bus 0) and 0x31/0x32 (bus 1) through `clock_gate` 0x9ff20408 = `0x3f101008 + 4 * id` (low nibble = request, bits 7:4 = state). Same registers as P105 |
| gates turned off before boot? | no evidence of it | the only caller of the WMR clock helper passes on = 1. Of the 29 direct `clock_gate` calls, every one that passes off (r1 = 0) has an immediate id (0x19, 0x1e, 0x1f, 0x24, 0x43), none of them 0x2f-0x32; the three that take the id from a table (0x9ff0b8e0, 0x9ff146c2, 0x9ff16240) all pass on = 1. Not proof: a direct PMGR store elsewhere would not show up in this search. The tools read the gates before any FMI access anyway |
| block reset | PMGR bit 31 | `h2fmi_device_reset` = 0x9ff04fc0: `WMR_CLOCK_RESET_DEVICE(bus)` (0x9ff225c8, ids 0x2f / 0x31) -> `clock_reset_device` 0x9ff1fdac, which pulses bit 31 of PMGR words (seen for id 6; the path for 0x2f was not traced), then FMI_CONTROL = 6. Same as what `nandctl recover` does on P105 |
| PPN geometry | 2 CAUs, 1064 blocks/CAU, 256 pages (128 SLC), 16448-byte pages, cau/block/page bits 4/11/8 | READ: ioreg `controllers` and `caus-ce 2`, `blocks-cau 0x428`, `#block-pages 256`, `#ce-blocks 2128`. Same as one P105 package, so `PPN_ROW` = page \| block << 8 \| cau << 19 carries over |
| read ID | `50 50 4E 01 05 05` ("PPN" v1.5.5) | READ: `device-readid` 5519054884944 = 0x05_05_01_4E_50_50 |
| firmware / ids | `040472P_HYNIX_B_`, manufacturer `AD 82 52 23 21 01` | READ: ioreg (P105's last byte is 02: its two packages) |
| disk0 | 8 000 000 000 bytes = 1 953 125 LBAs of 4 KB | READ: ioreg-service `Size` |
| timing, Toggle/DDR | inherited from iBoot | iBoot derives `FMC_IF_CTRL`, `FMC_TOGGLE_CTRL_1/2`, `FMC_DQS_TIMING_CTRL` from the device parameters (strings "Using DDR mode", 0x9ff35c5d...). The tools never write the Toggle/DQS registers (+0x70..0x78) outside `recover`. They do touch `FMC_IF_CTRL`: the status poll clears its bit 20, as iBEC does. Page reads and read ID put `FMC_ON`/`FMC_IF_CTRL` back afterwards; `getfeat`/`params` do not (P105 behaviour, unchanged). Whether iBoot's values suit S5L8947X's FMI clock is iBoot's choice, and iBoot reads this NAND with them at every boot |

### What changed (all guarded; P105 builds as before)

- `tools/nand/ppn.h`: `-DPPN_BOARD_J33I` selects `PPN_BUSES 1`,
  `PPN_CE_BITMAP 0x001`, `PPN_DISK0_BYTES 8e9`, `PPN_READ_ONLY_STRICT 1`.
  Without it: P105's values, unchanged.
- `tools/nand/ppn.c`: bus and CE checks against the bitmap; a second
  table of opcodes that are never sent (program 80/81/85/10/11/15, erase
  60/D0/D1, PPN boot-page program 8A/17, PPN erase 67, set-features EF/D5,
  sync/LUN reset FA/FC) checked against the allowlist at start-up and on every
  command; a board check against `/proc/device-tree/compatible` (the J33i
  build needs `apple,j33iap`; the P105 build refuses only `apple,j33iap`). In
  the strict build NAND reset (FF), set-feature, the bare E7 and the only
  host -> NAND transfer (`FMI_CONTROL 5`, a write to `FMI_DATA`) are not
  compiled in.
- `tools/nand/nandctl.c`: buses checked, not masked with `& 1` (so on P105 a
  bus number other than 0 or 1 is now refused instead of folded; 0 and 1 behave
  as before); `regs` and
  `ppninfo` loop over `PPN_BUSES`; `reset`, `sdr`, `ddr` not compiled in the
  strict build; new `ptab BUS CE [ROW]` decodes the `ndrG` flash partition table.
- `tools/nand/iosnand.c`: disk0 size from `ppn.h`; new `hfs [PART]` prints the
  volume header and the volume name, via `tools/nand/hfsvh.h` (read-only, tested on the
  host against an HFSX image and an HFS+ image made with `hdiutil`: signature,
  block size, total blocks and the volume names came out right; a zeroed image is
  refused).
- `cascadia`: passes `-DPPN_BOARD_J33I` to both tools when `BOARD=j33iap`.
  Before this, the J33i image shipped the P105 build, whose `ppninfo` would
  have driven FMI1.
- `mtd-apple-h2fmi.c`: per-compatible variant. `apple,s5l8940x-fmi` = P105,
  2 buses, as before. `apple,s5l8947x-fmi` = J33i, 1 bus, and probe first
  reads the PMGR gate words (4th window, mapped not requested) and refuses
  unless both read on, then refuses unless `FMC_ON` = 5 (DDR).
- `dts/j33iap.dts`: `flash-controller@1200000`, FMI0/FMC0/ECC0 plus the
  gate words, interrupt 33, `status = "disabled"`. Its own compatible only,
  so the two-bus match cannot bind.

### How the code guarantees no write or erase

1. One door. In `ppn.c` every command byte reaches `FMC_CMD` through
   `nand_cmd2()`. In the J33i build the preprocessed source has exactly
   one store to `FMC_CMD` (checked with `gcc -E`, 2026-10-22), and it is
   inside `nand_cmd2()`. P105's build keeps its two extra stores (E7 and EF,
   for the power state); the J33i build compiles both out.
2. Two tables. The allowlist (J33i: 90, 70, EE/E7, 92/97, 77/7D, 77, 7A,
   0A/37) is checked against the never-list at start-up. A mutant with
   `{ 0x80, 0x10 }` added to the allowlist exits with "opcode 80 10 is on
   both lists" before it opens `/dev/mem` (run on the host, 2026-10-22).
3. Nothing flows host -> NAND. The only `FMI_CONTROL = 5` and the only
   `FMI_DATA` store are in `ppn_set_power_state`, which the strict build does
   not compile (`nm`: no `ppn_set_power_state`, `ppn_nand_reset`,
   `ppn_cmd_e7` in the J33i `nandctl`).
4. NBD is read-only: `NBD_FLAG_READ_ONLY`, and `NBD_CMD_WRITE` gets EPERM.
   `ios mount` mounts `-o ro`.
5. The kernel driver has no `_write`, `_erase` or `MTD_WRITEABLE`, and
   `MTD_BLOCK` is off. Its allowlist still lists FF, which no code path in the
   driver sends.

What is not ruled out by construction: a *read* sequence that confuses the
PPN or the FMC (P105 saw that with two processes on one bus), which costs a
reboot, not data. The per-bus lock is still there.

### First on-box test

Preconditions: boot the fsboot chain (iBoot reads `/linux.kc` off System and
jumps; iOS, and so the `AppleIOPFMI` firmware, never runs). Not a kloader
boot out of a running iOS. One shell, nothing else touching the NAND.

Each step is read-only. Stop at the first surprise.

```
# 0. right binaries, right board
nandctl 2>&1 | tail -1        # "(built for j33iap, 1 bus, read-only)"
cat /proc/device-tree/compatible | tr '\0' ' '    # apple,j33iap ...

# 1. clocks and bus mode: registers only, no NAND command
peek r 0x3f1010c4 2                    # both words: low byte 0xff (requested on, is on)
nandctl regs                           # FMI0 only; FMC +000 (FMC_ON) = 00000005,
                                       # +070..078 = Toggle timing (P105: 00c40000 03020100 01011d0b)

# 2. identify the package
nandctl readid 0 0                     # DDR repeats each byte: 50 50 50 50 4e 4e 01 01 "PPPPNN.."
                                       # (the ID is 50 50 4E 01 05 05, "PPN" v1.5.5)
nandctl ppninfo                        # FMI0 CE0 firmware ... "040472P_HYNIX_B_"
nandctl params 0 0                     # "PPN Device Info"; 0x10: CAUs 2, 0x18: blocks 0x428,
                                       # pages 0x100, page size 0x4040

# 3. one page, twice, must be identical
nandctl page 0 0 0x000100 /tmp/p1      # CAU0 block 1 page 0: status 0x40 or 0x49
nandctl page 0 0 0x000100 /tmp/p2 && cmp /tmp/p1 /tmp/p2

# 4. the flash partition table
nandctl ptab 0 0                       # header "ndrG"; entries boot, nvrm, firm, fsys ...
nandctl ptab 0 0 1                     # page 1: the same table

# 5. FTL map, LwVM, the System volume header (first run reads every page's
#    metadata once: ~6 min for 2 dies, by P105's 714 s for 4)
IOSNAND_CACHE=/tmp/pages.v1 iosnand parts      # System ~1334 MB, Data ~6225 MB
IOSNAND_CACHE=/tmp/pages.v1 iosnand hfs System
```

What proves success:

- step 2: read ID `50 50 4E 01 05 05` (printed doubled in DDR, as above) and firmware `040472P_HYNIX_B_`,
  equal to the IORegistry. This proves the base, the gates and the bus mode in
  one go.
- step 3: status 0x40 (or 0x49 for an erased page), and `cmp` silent.
- step 4: `ndrG`, an `fsys` entry covering nearly all 2128 blocks (P105: start
  1, 2127 blocks).
- step 5: `iosnand parts` shows System about 1 399 549 952 bytes and Data about
  6 527 684 608 (ioreg sizes); `iosnand hfs System` prints signature `"HX"`
  (HFSX, version 5) or `"H+"` (version 4) with block size 4096 and
  block size x total blocks = 1 399 549 952, plus a volume name.
  P105's System was HFSX, named `Donner12H321.P105OS`. On a 12H847 Apple TV
  expect something like that with the J33i build name, GUESS. Then
  `ios mount` and `cat /mnt/ios/System/Library/CoreServices/SystemVersion.plist`
  should say 8.4.4 / 12H847 (the version the live ioreg came from).

RAM: the page table is about 13 MB, the LBA map 7.8 MB, plus 15.6 MB while
it is built. About 37 MB at peak, and the cache file is another 13 MB wherever
it lands in RAM (`/tmp` and `/var/lib/iosnand` are both RAM on the RAM root):
about 50 MB against the RAM root's ~88 MB free. On an NFS root, point
`IOSNAND_CACHE` there instead. It saves the 13 MB and survives a reboot (later
starts take about a second instead of a full scan).

The kernel driver is a later, separate step. Enable the node only after all
five steps passed: flip `status`, rebuild, boot. Look for `bus 0:
FMC_ON=0x00000005` and `2 dies, 1064 blocks of 4096 KiB each, read-only`.
Then read the same page through MTD (`dd if=/dev/mtd0 bs=16384 count=1
skip=256`, block 1 page 0) and check it against `/tmp/p1` with the 16 metadata
bytes at 0x400 of each 4112-byte chunk cut out.

### Risks, ranked

1. **FMI state on the fsboot path (medium likelihood, low cost).** The port
   assumes iBoot leaves FMI0 clocked, in DDR, with its timing, as iBEC does on
   P105. iBoot does read System over this bus at every fsboot boot, through
   the same `h2fmi` code, and no `clock_gate` call in iBoot switches FMI
   off (Evidence table). But a direct PMGR store before the jump would not
   show up in that search. Cost if wrong: the tools refuse (gates) or a command times out.
   No data at stake. Step 1 checks it before any NAND command.
2. **The IOP (low on fsboot, high on a kloader-from-iOS boot).** `AppleIOPFMI`
   drives FMI from the IOP while iOS runs. On fsboot iOS never starts, so the
   IOP is never loaded (GUESS, consistent with iBoot driving FMI itself).
   After a kloader boot out of a running iOS the IOP may still be running.
   Two masters on one FMC wedge it (P105) and could leave a half-done iOS
   write. **Do not run any of this after a kloader-from-iOS boot.** Nothing
   in the code can tell the two boot paths apart; this rule is on the operator.
3. **FMI1 (low now, was real).** Reading data on FMI1 wedges it (no chip,
   no DQS). The J33i build never maps it. Before this change the image
   shipped the P105 tools, and `nandctl ppninfo` would have driven it. Wedging
   is a host-side hang, not NAND damage; recovery is a reboot.
4. **A confused PPN (low).** Concurrent sequences, or one aborted halfway,
   can leave the PPN answering 0x11/0x51 (P105). The lock prevents the
   first. Recovery is a reboot through iBoot. The strict build cannot send
   FF, so it cannot drop the PPN to SDR (on P105 that only came back after a
   reboot).
5. **`nandctl recover` (low, avoid).** It pulses the FMI block reset (PMGR bit
   31 at 0x3f1010c4). That is iBoot's own mechanism (`h2fmi_device_reset`,
   Evidence table) and was verified on P105, but has not been run on J33i. It
   sends no NAND command. Use it only if a bus is already wedged, and prefer a reboot.
6. **Geometry or metadata assumptions (low, safe failure).** Stripe or sequence
   arithmetic could differ with 2 dies. The map only uses "highest sequence
   wins per LBA", and every read checks the LBA in the page's metadata, which
   catches data that moved since the scan. It does not catch everything:
   an LBA the scan missed reads back as zeros, and a wrong sequence order
   would serve an older copy of the right LBA. Both are wrong data, not a
   write. The checks against that are external: `fsck_hfs -n` on a dumped
   image, `codesign -v` on binaries, as on P105.
7. **Boot page layout (low, cosmetic).** `ptab` reads the boot page raw and
   drops the BCH bytes. In DDR the FMI may shift each sector (P105). If the
   header does not read `ndrG`, try ROW 1 and compare with `bootpage` output.
   That is a read, so nothing can be lost.

Confidence: high that the base, gates and page geometry are right (two
independent READ sources agree, and P105's verified sequences are
unchanged). High that no write or erase path exists in the J33i build
(structural, checked in the preprocessed source and the binary's symbols, plus
a mutation test). Medium that the first `readid` just works, because of the
untested assumption that iBoot leaves FMI0 clocked and in DDR on fsboot (risk 1).
