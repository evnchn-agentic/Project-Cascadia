#!/usr/bin/env bash
# atv-demo: assets, the ARM build and the host preview, into build/atv-demo/.
#
#   tools/atv-demo/build.sh            assets + static armhf binary (docker image cascadia-build)
#   tools/atv-demo/build.sh preview    also a host build and every screen as PNG in build/atv-demo/preview/
#
# Assets need Python with Pillow and qrcode; a uv venv in build/atv-demo/venv gets them
# (offline from uv's cache when it has them).  Font: JetBrains Mono, SIL OFL 1.1 (fonts/OFL.txt).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/atv-demo"
mkdir -p "$OUT/assets"

PY="$OUT/venv/bin/python"
if ! "$PY" -c 'import PIL, qrcode' 2>/dev/null; then
    command -v uv >/dev/null || { echo "build.sh: needs uv (or a $PY with Pillow + qrcode)" >&2; exit 1; }
    uv venv -q "$OUT/venv"
    uv pip install -q -p "$PY" --offline pillow qrcode 2>/dev/null || uv pip install -q -p "$PY" pillow qrcode
fi
"$PY" "$HERE/gen-assets.py" assets "$OUT/assets"

docker run --rm -v "$ROOT":/cascadia -w /cascadia cascadia-build \
    arm-linux-gnueabihf-gcc -O2 -static -mcpu=cortex-a9 -mfpu=neon -Wall -Wextra \
    -o build/atv-demo/atv-demo tools/atv-demo/atv-demo.c -lm
echo "ok: $OUT/atv-demo ($(wc -c < "$OUT/atv-demo" | tr -d ' ') bytes, static armhf)"

if [ "${1:-}" = preview ]; then
    cc -O2 -o "$OUT/atv-demo-host" "$HERE/atv-demo.c" -lm
    rm -rf "$OUT/preview"
    "$OUT/atv-demo-host" -a "$OUT/assets" --render-screens "$OUT/preview" >/dev/null
    "$PY" "$HERE/gen-assets.py" png "$OUT"/preview/*.ppm
    rm -f "$OUT"/preview/*.ppm
    echo "ok: $(ls "$OUT/preview" | wc -l | tr -d ' ') screens in $OUT/preview"
fi
