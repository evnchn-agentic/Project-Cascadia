#!/usr/bin/env bash
#
# kDFU over the network, for devices Legacy iOS Kit does not handle (AppleTV3,2).
# Copies kloader and the pwned iBSS to the jailbroken device over ssh and runs
# it; the device drops off the LAN and comes back on USB in DFU.  Then:
#
#   BOARD=j33iap ./cascadia flash --skip-pwn --no-uart \
#       --ibec build/firmware-j33i/iBEC.patched.autogo.plain.dfu
#
#   tools/kdfu-lan.sh [root@192.168.50.138]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${1:-root@192.168.50.138}"
PASS="${PASS:-alpine}"
KLOADER="${LIK:-$HOME/Legacy-iOS-Kit}/resources/kloader/kloader"
IBSS="${IBSS:-$ROOT/build/firmware-j33i/iBSS.pwned}"

# iOS 8's dropbear/OpenSSH only speaks the old algorithms.
OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
      -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group14-sha1
      -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa)

for f in "$KLOADER" "$IBSS"; do [ -s "$f" ] || { echo "missing $f" >&2; exit 1; }; done

sshpass -p "$PASS" scp -O "${OPTS[@]}" "$KLOADER" "$IBSS" "$HOST:/tmp/"
# The ssh session dies with the kernel, so fire and forget.
sshpass -p "$PASS" ssh "${OPTS[@]}" "$HOST" \
    "chmod +x /tmp/kloader; nohup /tmp/kloader /tmp/$(basename "$IBSS") >/dev/null 2>&1 &" || true

echo "==> kloader started; waiting for DFU on USB"
IREC="$(command -v irecovery || echo "${LIK:-$HOME/Legacy-iOS-Kit}/bin/macos/irecovery")"
for _ in $(seq 30); do
    if sudo "$IREC" -q 2>/dev/null | grep -q "MODE: DFU"; then echo "==> in kDFU"; exit 0; fi
    sleep 1
done
echo "no DFU device after 30 s -- is the micro-USB cable in?" >&2
exit 1
