#!/usr/bin/env bash
#
# One full Apple TV boot cycle, hands-off:
#   power-cycle into iOS with VBUS off -> VBUS on -> kloader -> patched iBoot in Recovery
#   (it lights HDMI; Recovery because USB is attached) -> staging bundle + loader.
#   Default PATH_MODE=dfu: kloader iBSS -> VBUS replug -> DFU -> iBoot over USB (proven).
#   PATH_MODE=direct: kloader iBoot straight from iOS: worked 1 of 2 tries (09:07 yes, 09:12 hung, no HDMI/USB).
#
#   tools/atv-cycle.sh [second-stage.dfu]     (dfu route only: what to send after iBSS)
#   STOP_AT=stage2 tools/atv-cycle.sh ...     stop once iBoot is in Recovery
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE2="${1:-$ROOT/build/firmware-j33i/iBoot.as-ibec.plain.dfu}"
PY="$ROOT/../.venv/bin/python -W ignore"
CTL="$PY $ROOT/tools/atv-ctl.py"
IREC="${LIK:-$HOME/Legacy-iOS-Kit}/bin/macos/irecovery"
ATV=root@192.168.50.138
O=(-J none -o ConnectTimeout=60 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
   -o LogLevel=ERROR -o ServerAliveInterval=3 -o ServerAliveCountMax=3
   -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group14-sha1
   -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa)

mode() { timeout 6 "$IREC" -m 2>/dev/null | tail -1; }
wait_mode() {  # wait_mode <substring> <seconds>
    for _ in $(seq "$2"); do mode | grep -q "$1" && return 0; sleep 1; done; return 1
}

$CTL ios-boot || exit 1

# /tmp is wiped on reboot, and sshd sometimes rejects the first logins: retry the upload.
PAYLOAD="$ROOT/build/firmware-j33i/iBoot.autogo"   # direct: the raw patched iBoot
[ "${PATH_MODE:-dfu}" = dfu ] && PAYLOAD="$ROOT/build/firmware-j33i/iBSS.pwned"
for f in "${LIK:-$HOME/Legacy-iOS-Kit}/resources/kloader/kloader" "$PAYLOAD"; do
    for _ in 1 2 3 4 5; do sshpass -p alpine scp -O "${O[@]}" "$f" "$ATV:/tmp/" 2>/dev/null && break; sleep 4; done
done
timeout 30 sshpass -p alpine ssh "${O[@]}" "$ATV" "chmod +x /tmp/kloader; /tmp/kloader /tmp/$(basename "$PAYLOAD")" >/dev/null 2>&1

if [ "${PATH_MODE:-dfu}" = dfu ]; then
    # kloader leaves the USB controller off the bus; a replug brings up the pwned iBSS as DFU.
    $CTL vbus off >/dev/null; sleep 3; $CTL vbus on >/dev/null
    wait_mode DFU 20 || { echo "no DFU after kloader (mode: $(mode))" >&2; exit 2; }
    echo "==> kDFU"
    timeout 60 "$IREC" -f "$STAGE2" >/dev/null 2>&1
fi
wait_mode Recovery 30 || { echo "iBoot did not reach Recovery (mode: $(mode))" >&2; exit 3; }
echo "==> iBoot up in Recovery (${PATH_MODE:-dfu} route)"
[ "${STOP_AT:-}" = stage2 ] && exit 0

timeout 120 "$IREC" -f "$ROOT/output/staging-bundle.bin" >/dev/null 2>&1; sleep 1
timeout 30 "$IREC" -f "$ROOT/output/staging-loader.bin" >/dev/null 2>&1
echo "==> bundle sent; bringing up the link"
bash "$ROOT/tools/host-link.sh"
