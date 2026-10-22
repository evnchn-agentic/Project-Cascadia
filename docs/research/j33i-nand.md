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
| Interface | Apple FMI (`flash_controller0@1200000`, `fmi,s5l8940x` / `s5l8920x`), one bus | READ: DTS node has six 4 KB reg windows (one FMI + FMC + ECC set), iPad has two sets |
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

- 1 bus (FMI0 at 0x31200000 per the DTS `reg` 0x1200000 + the 0x30000000 ADT
  base; confirm the base, GUESS) instead of 2. So 2 dies (CAUs) instead of 4.
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

- Confirm the FMI base address (ADT `reg` gives 0x1200000 relative to the
  SoC base; the iPad's 0x31200000 suggests the same, GUESS).
- Confirm how much free space the System partition has, if option (5) is wanted.
- Confirm whether the micro-USB port can ever be host-mode on this board; no
  evidence for it in the files read.
- Confirm RAM size (DTS says 512 MiB as a placeholder; the real value is
  patched at boot, GUESS 512 MB).
