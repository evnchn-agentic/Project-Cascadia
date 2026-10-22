# Getting the Apple TV 3 work upstream

Branch `j33i`, 57 commits on top of upstream `main` at 5089601 (2026-10-17). Upstream has not
moved since, so everything below applies cleanly today. Nothing has been pushed or posted.

## What upstream looks like (2026-10-22)

- One maintainer (rzxvx). His stated focus: iPad mini 1, iPad 9 (A13) and iPad 4, "with the
  primary focus being on the iPad 9" (issue 22, 2026-10-17). He asked to note the iPad 3 "for
  the future".
- He has merged one outside board port (PR 7, A6X / iPad 4: DTS + ADT parser). He has not yet
  merged eleven outside PRs from 2026-10-12..16, including an iPad 2 port (PR 21) and ten
  small P105 fixes. Review bandwidth, not willingness, is the bottleneck.
- GPL-2.0. Discussions are enabled.

So: an Apple TV 3 series will not be his priority, and a big PR will sit. Small, self-contained
PRs with evidence have the best odds. The fixes that also help the iPad mini 1 are the ones
he has a reason to take.

## The 57 commits in three buckets

### A. Shared fixes that help P105 too (send first, one PR each)

| Commit | What | P105 effect | Risk without a P105 test |
|---|---|---|---|
| 5f567bb (loader part) | staging loader stack sat inside the zImage; L2 flush polled way 0 only, no sync | a latent memory-corruption bug on every board | low: plain bug fix, Codex-found |
| b75be00 | pinctrl: pins iBoot left in a trigger mode are put back to input at probe | prevents an AIC IRQ livelock after any warm handoff | low-medium |
| 922c26c | USB PHY: run iBoot's PHY bring-up when iBoot left none (guarded by the uotgtune2 check) | inert on P105's normal route; enables boots where iBoot never started USB | low: guarded |
| e45ef6f (extractor part) | `extract-sgx-firmware.py --kernelcache`: take a decrypted kernelcache directly, relocate the kext's kmod_info | lets anyone extract from a kernelcache without the IPSW tooling | low |
| 94dadba | clocksource: prefer the AIC timebase when it moves | changes P105's clocksource | **needs a P105 tester**; offer as opt-in or skip |

### B. Apple TV 3 board support (a short series, after A lands or alongside)

1. `dts/j33iap.dts`, the `BOARD=` switch in `cascadia`, `scripts/build-firmware-j33i.sh`,
   `scripts/patch-ibec-autogo.py` constants: boots over USB like P105.
2. Drivers, each its own commit: `drivers/net/ethernet/apple/s5l-fe.c` (Ethernet),
   `drivers/input/misc/apple-tv-mcu.c` (IR remote, LED), `sound/soc/apple/s5l-hdmi.c` (HDMI
   audio), `arch/arm/mach-apple/j33i_display.c` (1080p), the J33i SGX node.
3. Stage 2's J33i blocks (Wi-Fi REG_ON, eth0 MAC + DHCP, banner, ALSA default, reboot).
4. The research notes in `docs/research/j33i-*.md` (they are the evidence and match his style).
5. The cable-free boot (kloader -> iBEC -> iBoot fsboot of `/linux.kc`, Wake-on-LAN) as its own
   PR: it needs a file on the iOS system partition, which is a policy call for him.

Clean-ups before B goes out:
- Give J33i its own config fragment (`config/j33iap.config`) instead of adding
  `S5L_FE`, `INPUT_APPLE_TV_MCU`, `SND_SOC_APPLE_S5L_HDMI` to the shared `p105ap.config`.
- Regenerate the touched parts of `patches/tree/0001-cascadia.patch` on top of his latest.
- Drop the per-unit Bluetooth address from the DT (derive it like eth0's MAC).

### C. Stays here (bench and debugging, not product)

`tools/atv-ctl.py` (smart plug + USB relay), `tools/kdfu-lan.sh`, `tools/atv-cycle.sh`,
`tools/atv-direct-trial.sh`, `tools/atv-kc-probe.sh`, `j33i_probe.c` (reset probes),
`tools/audio/hdmitone.c`, the ES02 capture scripts. They encode this bench, not the device.

## Before anything public

- Scrub: home-LAN addresses, the unit's MAC/BD addresses, hostnames, board-card references
  and internal names in commit messages and comments. Do it on a fresh branch built by
  cherry-pick + squash; `j33i` stays as the working history.
- Re-run the build on the cleaned branch and boot it once on the box.
- Codex pass on each PR diff (house rule), and a supercomment body: what, evidence, how tested,
  what was not tested (no P105 hardware here).

## Order and decision points (each one needs evnchn's go)

1. **Ask first, cheaply.** A GitHub Discussion: "Apple TV 3 (J33i, S5L8947X) runs on Cascadia:
   would you take it, and in what shape?" with a link to a public fork branch. Costs him a
   minute; tells us whether B is welcome.
2. **Bucket A PRs** regardless of the answer: they help his main board.
3. **If he says yes:** bucket B as a short series, one PR at a time, waiting for each merge.
4. **If silence for ~2 weeks, or no:** a distant fork.

## The distant fork

- A public fork whose `main` tracks upstream `main` and adds the J33i series on top, rebased
  when upstream moves (most of our changes are additive, so conflicts stay small: the shared
  hot spots are `p105-stage2`, the `cascadia` script and `0001-cascadia.patch`).
- README up top: what works on the Apple TV 3, how to boot it, and that it follows Project
  Cascadia; credit and link upstream. Keep sending bucket A fixes upstream as they come up.
- Keeping it in sync is a standing chore with no deadline: a candidate for the bus, triggered
  by an upstream push.

## Why keep working ahead here

The bench here closes the loop without hands: power and USB relays, Wake-on-LAN boot, HDMI and
audio capture, and a positive control from stock iOS on the same box. That is how 1080p, HDMI
audio, the remote MCU and the GPU got done in a day. Upstream is a single maintainer on an iPad
9. The edge is ours to keep; the plan above is how it pays back.
