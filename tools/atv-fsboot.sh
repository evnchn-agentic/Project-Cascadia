#!/usr/bin/env bash
#
# Boot Linux from the Apple TV's own storage: kloader chain.kload from iOS (iBEC does the init,
# then jumps to iBoot.fsboot stashed in /vram, tools/mk-chain-kloader.py), and iBoot autoboots
# /linux.kc (tools/mk-fake-kc.py) off the system partition. Nothing is uploaded over USB; VBUS
# stays on only so the USB link to Linux can confirm the boot.
#
#   tools/atv-fsboot.sh install     write /linux.kc to the system partition (needs evnchn's yes)
#   tools/atv-fsboot.sh remove      rm /linux.kc
#   tools/atv-fsboot.sh [tries]     boot (default 6 tries); refuses if /linux.kc is missing
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CTL="$ROOT/../.venv/bin/python -W ignore $ROOT/tools/atv-ctl.py"
FW="$ROOT/build/firmware-j33i"
ATV=root@192.168.50.138
O=(-J none -o ConnectTimeout=40 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
   -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group14-sha1
   -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa)
atv() { for _ in 1 2 3 4 5; do sshpass -p alpine ssh "${O[@]}" "$ATV" "$@" && return 0; sleep 5; done; return 1; }
put() { for _ in 1 2 3 4 5; do sshpass -p alpine scp -O "${O[@]}" "$1" "$ATV:$2" 2>/dev/null && return 0; sleep 5; done; return 1; }

case "${1:-}" in
install)
    kc="$FW/linux.kc"
    python3 "$ROOT/tools/mk-fake-kc.py" "$ROOT/output/staging-bundle.bin" "$ROOT/../work/kernelcache.release.j33i" "$kc" || exit 1
    put "$kc" /linux.kc.new && atv "mv -f /linux.kc.new /linux.kc && sync && ls -l /linux.kc && md5 /linux.kc 2>/dev/null || true"
    md5 -q "$kc"; exit ;;
remove) atv "rm -f /linux.kc /linux.kc.new && sync"; exit ;;
esac

python3 "$ROOT/tools/patch-iboot-fsboot.py" ${PATCH_FLAGS:-} "$FW/iBoot.autogo" "$FW/iBoot.fsboot" >/dev/null || exit 1
python3 "$ROOT/tools/mk-chain-kloader.py" "$FW/iBEC.patched" "$FW/iBoot.fsboot" "$FW/chain.kload" >/dev/null || exit 1
for try in $(seq "${1:-6}"); do
    echo "==> try $try"
    $CTL ios-boot >/dev/null || continue
    atv "test -s /linux.kc" || { echo "no /linux.kc on the device (run: $0 install)" >&2; exit 2; }
    put "${LIK:-$HOME/Legacy-iOS-Kit}/resources/kloader/kloader" /tmp/ && put "$FW/chain.kload" /tmp/ || continue
    timeout 20 sshpass -p alpine ssh "${O[@]}" "$ATV" "chmod +x /tmp/kloader; /tmp/kloader /tmp/chain.kload" >/dev/null 2>&1
    # host-link.sh waits up to 180 s for the gadget; a hung iBoot never brings it up.
    timeout 200 bash "$ROOT/tools/host-link.sh" && { echo "==> Linux up from /linux.kc (try $try)"; exit 0; }
done
echo "no boot after ${1:-6} tries" >&2; exit 1
