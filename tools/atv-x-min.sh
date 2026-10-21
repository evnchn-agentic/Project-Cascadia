#!/usr/bin/env bash
#
# Minimal X11 on the Apple TV's HDMI under Linux, with no network on the box and no change
# to this host's networking: packages are fetched here (Alpine armhf, signed), pushed over
# the USB link, installed offline, then Xorg runs on the simplefb framebuffer with twm.
# The root is RAM: rerun after every Linux boot. Full XFCE (~337 packages) needs a disk.
#
#   tools/atv-x-min.sh [package ...]     (default: a clock, eyes and a terminal)
set -euo pipefail

PKGS=(xorg-server xf86-video-fbdev xf86-input-evdev xinit xterm twm font-misc-misc
      xsetroot xclock xeyes "$@")
CACHE="${CACHE:-$HOME/atv3-linux/apks-min}"
L=(ssh -J none -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
   -o LogLevel=ERROR root@10.55.0.2)

mkdir -p "$CACHE"
# The rootfs is armhf (ARMv6+VFP userland), hence the arm/v6 image, not arm/v7.
docker run --rm --platform linux/arm/v6 -v "$CACHE":/out alpine:3.24 \
    sh -c "apk update -q && apk fetch -q -R -o /out ${PKGS[*]}"
# Alpine's own signing keys, so apk verifies rather than --allow-untrusted.
docker run --rm --platform linux/arm/v6 alpine:3.24 tar cf - -C /etc/apk keys | "${L[@]}" 'tar xf - -C /etc/apk'
( cd "$CACHE" && tar cf - *.apk ) | "${L[@]}" 'mkdir -p /tmp/apks && tar xf - -C /tmp/apks'
"${L[@]}" 'apk add --no-network --quiet /tmp/apks/*.apk 2>&1 | grep -v "opening from cache" || true; rm -rf /tmp/apks'

"${L[@]}" 'mkdir -p /etc/X11/xorg.conf.d
cat > /etc/X11/xorg.conf.d/10-fbdev.conf <<EOF
Section "Device"
  Identifier "fb"
  Driver "fbdev"
  Option "fbdev" "/dev/fb0"
EndSection
EOF
cat > /root/.xinitrc <<EOF
xsetroot -solid steelblue &
xclock -geometry 300x300+60+60 &
xeyes -geometry 300x200+420+60 &
xterm -geometry 90x20+60+420 -e sh -c "uname -a; cat /proc/device-tree/model; echo; free -m; exec sh" &
exec twm
EOF
pkill Xorg 2>/dev/null; (nohup startx -- -nolisten tcp vt1 >/tmp/x.log 2>&1 &); sleep 8; pgrep -l Xorg'
