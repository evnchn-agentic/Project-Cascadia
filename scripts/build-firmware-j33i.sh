#!/usr/bin/env bash
#
# AppleTV3,2 (J33i) boot chain from the 12H847 IPSW (the last build with public
# iBSS/iBEC/iBoot keys).  Run on the host; patching happens in the build image.
#
#   iBSS.pwned                   kloader payload (signature patch)
#   iBEC.patched.autogo.plain.dfu  iBEC + autogo -- boots Linux, but iBEC has no HDMI driver
#   iBoot.as-ibec.plain.dfu      full iBoot + autogo, packed as an iBEC: lights HDMI (default)
#   sgx543.fw                    the GPU microkernel, from the decrypted kernelcache
#                                (KC_J33I, default ../work/kc.j33i.macho), if it is there
#   kernelcache.12H847.macho     that kernelcache, copied into the tree: tools/sgx/mkpack.py
#                                runs the GPU kext's render-target code from it, in the
#                                build image, which sees only this tree
#   gl-event.pds                 the 3D pass's event PDS program, a template in the GL
#                                driver (dyld shared cache, root filesystem), for the
#                                sgx2d pack and Mesa: byte-identical to the iPad's
#
#   IPSW=path/to/AppleTV3,2_8.4.2_12H847_Restore.ipsw scripts/build-firmware-j33i.sh
#   ONLY=gpu scripts/build-firmware-j33i.sh     the last three only (./cascadia build
#                                               and gpu run it so)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IPSW="${IPSW:-$ROOT/../ipsw/AppleTV3,2_8.4.2_12H847_Restore.ipsw}"
OUT="$ROOT/build/firmware-j33i"
BOOTARGS="cs_enforcement_disable=1 debug=0x14"
mkdir -p "$OUT"

if [ "${ONLY:-}" != gpu ]; then
unzip -o -q -j "$IPSW" 'Firmware/dfu/iBSS.j33i.RELEASE.dfu' 'Firmware/dfu/iBEC.j33i.RELEASE.dfu' \
    'Firmware/all_flash/all_flash.j33iap.production/iBoot.j33i.RELEASE.img3' -d "$OUT"

# Keys:DonnerTideUpdate 12H847 (AppleTV3,2), theapplewiki.com
docker run --rm -v "$ROOT":/cascadia -w /cascadia/build/firmware-j33i "${IMAGE:-cascadia-build}" bash -c "
set -e
d=/cascadia/scripts/img3decrypt.py
python3 \$d iBSS.j33i.RELEASE.dfu iBSS.j33i.bin 21216b79c11c5a0f070a9a737ad4a012 b1c62cff5b0605f71aa3294b87a1569d7543c033a758532ddc3a444fb41c2140
python3 \$d iBEC.j33i.RELEASE.dfu iBEC.j33i.bin f9cf1f7ede9e8bd901313ada7d7de708 85e62339808553bbfce7e441c79d83fedd7fa0bd9804b2629aa618f740737c56
python3 \$d iBoot.j33i.RELEASE.img3 iBoot.j33i.bin fd409ae56794dec1d25e1876800895c3 025ad2f41b1416b4c72dda869571d8115dd95c19ef1ff430ec8e0c14740bd442
iBoot32Patcher iBSS.j33i.bin iBSS.pwned --rsa >/dev/null
iBoot32Patcher iBEC.j33i.bin iBEC.patched --rsa -b '$BOOTARGS' >/dev/null
iBoot32Patcher iBoot.j33i.bin iBoot.patched --rsa -b '$BOOTARGS' >/dev/null
python3 /cascadia/scripts/patch-ibec-autogo.py --board j33i iBEC.patched iBEC.autogo >/dev/null
python3 /cascadia/scripts/patch-ibec-autogo.py --board j33i-iboot iBoot.patched iBoot.autogo >/dev/null
python3 /cascadia/scripts/img3pack.py iBEC.j33i.RELEASE.dfu iBEC.autogo iBEC.patched.autogo.plain.dfu
python3 /cascadia/scripts/img3pack.py iBEC.j33i.RELEASE.dfu iBoot.autogo iBoot.as-ibec.plain.dfu
md5sum iBSS.pwned iBEC.autogo iBoot.autogo"
fi

# The GPU microkernel: on the host (the kernelcache lives outside this tree, and
# --kernelcache needs nothing but python3).  Byte-identical to the iPad's.
KC_J33I="${KC_J33I:-$ROOT/../work/kc.j33i.macho}"
if [ -f "$KC_J33I" ]; then
    python3 "$ROOT/scripts/extract-sgx-firmware.py" --kernelcache "$KC_J33I" "$OUT/sgx543.fw"
else
    echo "note: no $KC_J33I -- no GPU microkernel (scripts/extract-sgx-firmware.py --kernelcache)"
fi

if [ -f "$KC_J33I" ]; then
    cp "$KC_J33I" "$OUT/kernelcache.12H847.macho"
fi

# The 3D pass's event program: 0xa8 bytes of IMGSGX543GLDriver's __TEXT,__const
# (+0x2328, VA 0x202a9bd8), at 0x2a9bd8 in 12H847's dyld_shared_cache_armv7 --
# 0x46ebd8 in the iPad's 12H321 cache, the same bytes (the hash below is
# tools/sgx/pds.py's EVENT_SHA256, checked again there).  The root filesystem
# key: Keys:DonnerTideUpdate 12H847 (AppleTV3,2), theapplewiki.com.
ROOTFS_DMG=048-37456-047.dmg
ROOTFS_KEY=8d96c4efabe8cab91ce7bb531bb8fa78de0c4ab156952dcf29ca83b2e968b4aadd038d81
if [ -f "$IPSW" ]; then
    IPSW="$(cd "$(dirname "$IPSW")" && pwd)/$(basename "$IPSW")"     # docker -v wants it absolute
    docker run --rm -v "$ROOT":/cascadia -v "$IPSW":/ipsw.ipsw:ro -w /cascadia "${IMAGE:-cascadia-build}" \
        python3 scripts/rootfs-extract.py /ipsw.ipsw "$ROOTFS_DMG" "$ROOTFS_KEY" \
        "/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7@2a9bd8+a8" build/firmware-j33i/gl-event.pds
    echo "a1b855d41993968b40005e886f7ac11c8c3abb42aa0987e005eec4e6cb2a2285  $OUT/gl-event.pds" \
        | shasum -a 256 -c --quiet - || { echo "error: gl-event.pds is not the GL driver's event program" >&2; rm -f "$OUT/gl-event.pds"; exit 1; }
else
    echo "note: no $IPSW -- no gl-event.pds"
fi
