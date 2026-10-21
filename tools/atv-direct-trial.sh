#!/usr/bin/env bash
#
# How often does an image kloaded straight from iOS reach Recovery on USB? For bisecting the
# direct-iBoot hang (lands ~1 in 4). Each try: ios-boot, kloader <image>, watch USB for 20 s,
# replug VBUS, watch 20 s more. Nothing is written but kloader+image in iOS /tmp.
#
#   tools/atv-direct-trial.sh <image> [tries]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CTL="$ROOT/../.venv/bin/python -W ignore $ROOT/tools/atv-ctl.py"
IREC="${LIK:-$HOME/Legacy-iOS-Kit}/bin/macos/irecovery"
IMG="$1"; ATV=root@192.168.50.138
O=(-J none -o ConnectTimeout=40 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
   -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group14-sha1
   -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa)
put() { for _ in 1 2 3 4 5; do sshpass -p alpine scp -O "${O[@]}" "$1" "$ATV:/tmp/" 2>/dev/null && return 0; sleep 5; done; return 1; }
seen() { for _ in $(seq "$1"); do timeout 5 "$IREC" -m 2>/dev/null | grep -q Recovery && return 0; sleep 1; done; return 1; }

ok=0; n=0
for try in $(seq "${2:-4}"); do
    $CTL ios-boot >/dev/null || { echo "try $try: no iOS"; continue; }
    put "${LIK:-$HOME/Legacy-iOS-Kit}/resources/kloader/kloader" && put "$IMG" || { echo "try $try: upload failed"; continue; }
    timeout 20 sshpass -p alpine ssh "${O[@]}" "$ATV" "chmod +x /tmp/kloader; /tmp/kloader /tmp/$(basename "$IMG")" >/dev/null 2>&1
    n=$((n + 1))
    if seen 20; then r=landed
    else $CTL vbus off >/dev/null; sleep 3; $CTL vbus on >/dev/null; seen 20 && r="landed after replug" || r=HUNG; fi
    [ "$r" = HUNG ] || ok=$((ok + 1))
    echo "try $try: $r"
done
echo "$(basename "$IMG"): $ok of $n reached Recovery"
