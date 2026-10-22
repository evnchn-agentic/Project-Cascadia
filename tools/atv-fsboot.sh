#!/usr/bin/env bash
#
# Boot Linux from the Apple TV's own storage: kloader chain.kload from iOS (iBEC does the init,
# then jumps to iBoot.fsboot stashed in /vram, tools/mk-chain-kloader.py), and iBoot autoboots
# /linux.kc (tools/mk-fake-kc.py) off the system partition. Nothing is uploaded over USB.
# kloader sleeps iOS and runs the image on WAKE. WAKE=lan (default) wakes it with a Wake-on-LAN
# packet to iOS's en0, USB power off throughout, and finds Linux on the LAN (DHCP on eth0).
# WAKE=usb wakes it with a VBUS edge and confirms over the USB link, as before.
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

# PATCH_FLAGS defaults to --direct: iBoot never starts its USB stack (a live one kills Linux
# within ~2 s); Linux's PHY driver runs iBoot's PHY bring-up itself.
PATCH_FLAGS="${PATCH_FLAGS:---direct}"
python3 "$ROOT/tools/patch-iboot-fsboot.py" ${PATCH_FLAGS:-} "$FW/iBoot.autogo" "$FW/iBoot.fsboot" >/dev/null || exit 1
python3 "$ROOT/tools/mk-chain-kloader.py" "$FW/iBEC.patched" "$FW/iBoot.fsboot" "$FW/chain.kload" >/dev/null || exit 1
for try in $(seq "${1:-6}"); do
    echo "==> try $try"
    $CTL ios-boot >/dev/null || continue
    atv "test -s /linux.kc" || { echo "no /linux.kc on the device (run: $0 install)" >&2; exit 2; }
    put "${LIK:-$HOME/Legacy-iOS-Kit}/resources/kloader/kloader" /tmp/ && put "$FW/chain.kload" /tmp/ || continue
    mac=$(atv "ifconfig en0" | awk '/ether/{print $2}')
    [ "${WAKE:-lan}" = lan ] && $CTL vbus off >/dev/null
    timeout 20 sshpass -p alpine ssh "${O[@]}" "$ATV" "chmod +x /tmp/kloader; /tmp/kloader /tmp/chain.kload" >/dev/null 2>&1
    if [ "${WAKE:-lan}" = lan ]; then
        sleep 20
        python3 -c 'import socket,sys; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET,socket.SO_BROADCAST,1); [s.sendto(b"\xff"*6+bytes.fromhex(sys.argv[1].replace(":",""))*16,("255.255.255.255",9)) for _ in range(5)]' "$mac"
        for _ in $(seq 12); do
            sleep 10
            for i in $(seq 2 254); do ping -c1 -t1 192.168.50.$i >/dev/null 2>&1 & done; wait
            for ip in $(arp -an | grep -oE '192\.168\.50\.[0-9]+' | sort -u); do
                timeout 6 ssh -F /dev/null -i ~/.ssh/id_rsa -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=3 \
                    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@$ip \
                    'grep -q AppleTV3 /proc/device-tree/model' 2>/dev/null && echo "$ip" > "$FW/.linux-ip" &
            done; wait
            [ -s "$FW/.linux-ip" ] && { echo "==> Linux up from /linux.kc at $(cat "$FW/.linux-ip") (try $try)"; rm -f "$FW/.linux-ip"; exit 0; }
        done
    else
        # A VBUS edge wakes the image and later enumerates the gadget.
        sleep 30
        $CTL vbus off >/dev/null; sleep 3; $CTL vbus on >/dev/null
        timeout 200 bash "$ROOT/tools/host-link.sh" && { echo "==> Linux up from /linux.kc (try $try)"; exit 0; }
    fi
done
echo "no boot after ${1:-6} tries" >&2; exit 1
