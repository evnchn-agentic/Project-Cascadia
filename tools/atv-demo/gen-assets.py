#!/usr/bin/env python3
"""Build-time assets for atv-demo (run on the host by build.sh).

  gen-assets.py assets OUTDIR    fonts.bin (pre-rasterised JetBrains Mono), qr.txt
                                 (the sound clip is synthesized by atv-demo itself, at run time:
                                 a 2 MB WAV in the image would not compress)
  gen-assets.py png PPM...       PPM -> PNG next to each file (for the preview screens)

Needs Pillow (FreeType) and qrcode; build.sh makes a uv venv with both.

fonts.bin: the glyphs are rasterised here, with FreeType, rather than on the box
with stb_truetype: the box then only blends 8-bit coverage, which is cheaper on
one Cortex-A9, and no TrueType code runs there at all.  JetBrains Mono is OFL-1.1
(fonts/OFL.txt).
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(HERE, "fonts")

# (id, file, pixel size).  atv-demo.c looks faces up by id.
FACES = [
    ("title", "JetBrainsMono-Bold.ttf", 26),
    ("hero",  "JetBrainsMono-ExtraBold.ttf", 76),
    ("lead",  "JetBrainsMono-Regular.ttf", 28),
    ("cardt", "JetBrainsMono-Bold.ttf", 36),
    ("cards", "JetBrainsMono-Regular.ttf", 22),
    ("body",  "JetBrainsMono-Regular.ttf", 26),
    ("med",   "JetBrainsMono-Medium.ttf", 28),
    ("big",   "JetBrainsMono-Bold.ttf", 56),
    ("label", "JetBrainsMono-Bold.ttf", 18),
    ("foot",  "JetBrainsMono-Medium.ttf", 22),
    ("small", "JetBrainsMono-Regular.ttf", 22),
]
EXTRA = "·•—–→←…×°’“”↑↓é"
CHARS = [chr(c) for c in range(32, 127)] + list(EXTRA)


def gen_fonts(out):
    from PIL import Image, ImageDraw, ImageFont
    blob = bytearray(b"ATVF")
    blob += struct.pack("<I", len(FACES))
    for fid, fname, px in FACES:
        font = ImageFont.truetype(os.path.join(FONTS, fname), px)
        asc, desc = font.getmetrics()
        notdef = font.getmask("￿").getbbox()
        glyphs, bitmaps = [], bytearray()
        for ch in CHARS:
            if ch != " " and ch in EXTRA:
                # skip what the font does not have (it would draw .notdef)
                m = font.getmask(ch)
                if m.getbbox() == notdef and font.getlength(ch) == font.getlength("￿"):
                    continue
            adv = font.getlength(ch)
            x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
            w, h = max(0, x1 - x0), max(0, y1 - y0)
            off = len(bitmaps)
            if w and h:
                im = Image.new("L", (w, h), 0)
                ImageDraw.Draw(im).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
                bitmaps += im.tobytes()
            glyphs.append((ord(ch), w, h, x0, y0, int(round(adv * 64)), off))
        name = fid.encode().ljust(8, b"\0")
        blob += name + struct.pack("<HhhH", px, asc, desc, len(glyphs))
        for g in glyphs:
            blob += struct.pack("<IhhhhiI", *g)
        blob += struct.pack("<I", len(bitmaps)) + bitmaps
    with open(os.path.join(out, "fonts.bin"), "wb") as f:
        f.write(blob)
    return len(blob)


# -------------------------------------------------------------------- QR ---
QR_URL = "https://github.com/evnchn-agentic/Project-Cascadia"


def gen_qr(out):
    import qrcode
    q = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M, border=0)
    q.add_data(QR_URL)
    q.make(fit=True)
    m = q.get_matrix()
    with open(os.path.join(out, "qr.txt"), "w") as f:
        f.write("%d %s\n" % (len(m), QR_URL))
        for row in m:
            f.write("".join("1" if c else "0" for c in row) + "\n")
    return len(m)


def ppm_to_png(paths):
    from PIL import Image
    for p in paths:
        im = Image.open(p)
        im.save(os.path.splitext(p)[0] + ".png", optimize=False, compress_level=6)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "assets":
        out = sys.argv[2]
        os.makedirs(out, exist_ok=True)
        print("fonts.bin %d bytes" % gen_fonts(out))
        print("qr.txt %d modules" % gen_qr(out))
    elif len(sys.argv) >= 3 and sys.argv[1] == "png":
        ppm_to_png(sys.argv[2:])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
