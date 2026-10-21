#!/usr/bin/env bash
#
# Boot one test kernelcache through the fsboot chain and report whether the box reset or hung.
# A probe kc ends in iBoot's platform_reset (mk-fake-kc.py TRAMP=...): after a reset the stock
# iBoot holds in Recovery (USB attached) with "Boot Failure Count: 0"; a hang shows nothing on
# USB even after a replug. Installs <kc> as /linux.kc (card 917's file) and puts the real one back.
#
#   tools/atv-kc-probe.sh <test.kc>
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CTL="$ROOT/../.venv/bin/python -W ignore $ROOT/tools/atv-ctl.py"
FW="$ROOT/build/firmware-j33i"
IREC="${LIK:-$HOME/Legacy-iOS-Kit}/bin/macos/irecovery"
ATV=root@192.168.50.138
O=(-J none -o ConnectTimeout=40 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
   -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group14-sha1
   -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa)
atv() { for _ in 1 2 3 4 5; do sshpass -p alpine ssh "${O[@]}" "$ATV" "$@" && return 0; sleep 5; done; return 1; }
put() { for _ in 1 2 3 4 5; do sshpass -p alpine scp -O "${O[@]}" "$1" "$ATV:$2" 2>/dev/null && return 0; sleep 5; done; return 1; }
install_kc() { put "$1" /linux.kc.new && atv "mv -f /linux.kc.new /linux.kc && sync"; }

python3 "$ROOT/tools/patch-iboot-fsboot.py" "$FW/iBoot.autogo" "$FW/iBoot.fsboot" >/dev/null || exit 1
python3 "$ROOT/tools/mk-chain-kloader.py" "$FW/iBEC.patched" "$FW/iBoot.fsboot" "$FW/chain.kload" >/dev/null || exit 1
$CTL ios-boot >/dev/null || exit 1
install_kc "$1" || exit 1
put "${LIK:-$HOME/Legacy-iOS-Kit}/resources/kloader/kloader" /tmp/ && put "$FW/chain.kload" /tmp/ || exit 1
timeout 20 sshpass -p alpine ssh "${O[@]}" "$ATV" "chmod +x /tmp/kloader; /tmp/kloader /tmp/chain.kload" >/dev/null 2>&1
sleep 90
$CTL vbus off >/dev/null; sleep 3; $CTL vbus on >/dev/null; sleep 12
if timeout 5 "$IREC" -m 2>/dev/null | grep -q Recovery; then
    con=$( (sleep 2; echo /exit) | timeout 15 "$IREC" -s 2>&1 | grep -E 'Boot Failure|Delaying|Kernelcache|Entering' | tr '\n' ' ')
    echo "RESET (Recovery): $con"
else
    echo "HUNG (nothing on USB after replug)"
fi
# Put the real /linux.kc back.
$CTL ios-boot >/dev/null && install_kc "$FW/linux.kc" && atv "ls -l /linux.kc"
