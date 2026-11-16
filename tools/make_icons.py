#!/usr/bin/env python3
"""Banjo-Kazooie-X: the Xbox dashboard's two pictures, the title image (128x128) beside
the game and the save image (64x64) beside its save game.

Both are to be the game's own Jiggy (the HUD's sprite/ASSET_80D_LIVE_JIGGY), scaled
up pixel for pixel on a dark ground, as Ocarina-Of-Time-X does with its Ocarina icon.
Nothing of it is committed: the picture is read from data/bk.o2r (made from your
ROM), and the BMPs, the .rdf and the DXT1 .xbx the RXDK's bundler makes of them land
in build/gen/icons. tools/build.py runs this before imagebld, which puts them in the
XBE (/TITLEIMAGE, /DEFAULTSAVEIMAGE).

The Jiggy is a BK sprite (Torch's BKSprite: chunks in the N64's texture formats), not
a plain texture; until its decoder is here (read_icon), the XBE goes without them.

    python tools/make_icons.py
"""
import os
import struct
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARCHIVE = os.path.join(ROOT, "data", "bk.o2r")
OUT = os.path.join(ROOT, "build", "gen", "icons")
ICON = "assets/sprite/ASSET_80D_LIVE_JIGGY"
ICON_FRAME = 0 # the HUD's turning Jiggy: the frame that faces you
GROUND = (12, 16, 36)
RXDK = os.environ.get("RXDK_ROOT") or os.path.join(os.environ.get("ProgramData") or r"C:\ProgramData", "RXDK")
RDF = """// Dashboard %(what)s (%(size)dx%(size)d), from tools/make_icons.py: DXT1 is the only format the dashboard draws.

out_version        XPR0
out_packedresource %(name)s.xbx
out_header         %(name)s.h

Texture %(tex)s
{
   Source %(name)s.bmp
   Format D3DFMT_DXT1
   Levels 1
}
"""


def read_texture(z, name):
    """A LUS texture (V0: a 64-byte resource header, then type, width, height, size): (type, w, h, data)."""
    raw = z.read(name)
    kind, w, h, size = struct.unpack_from("<IIII", raw, 64)
    return kind, w, h, raw[80:80 + size]


def read_icon(frame=ICON_FRAME):
    """A frame of the Jiggy as (width, height, [[(r, g, b, a)]]), or None if the archive hasn't it. Torch splits
    the sprite into a texture a frame (4-bit palette, 32x32) and its palette (16 colours, RGBA5551, big-endian)."""
    with zipfile.ZipFile(ARCHIVE) as z:
        tex, tlut = "%s_%d_0" % (ICON, frame), "%s_%d_TLUT" % (ICON, frame)
        if tex not in z.namelist() or tlut not in z.namelist():
            return None
        kind, w, h, data = read_texture(z, tex)
        pkind, pw, ph, pdata = read_texture(z, tlut)
    if kind != 3 or pkind != 2 or len(data) < w * h // 2:  # (Palette4bpp, RGBA16bpp)
        return None
    palette = []
    for i in range(0, len(pdata), 2):
        c = (pdata[i] << 8) | pdata[i + 1]
        palette.append(tuple((c >> s & 31) * 255 // 31 for s in (11, 6, 1)) + ((c & 1) * 255,))
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            b = data[(y * w + x) // 2]
            row.append(palette[(b >> 4) if x % 2 == 0 else (b & 15)])
        rows.append(row)
    return w, h, rows


def write_bmp(path, size, pixel):
    """A 24-bit BMP, `size` square, pixel(x, y) -> (r, g, b)."""
    row = size * 3
    pad = (4 - row % 4) % 4
    data = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            r, g, b = pixel(x, y)
            data += bytes((b, g, r))
        data += bytes(pad)
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, size, size, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(hdr + info + data)
    os.replace(tmp, path)


def main():
    """The two .xbx paths (title image, save image), or None if there's no archive to take the icon from."""
    if not os.path.isfile(ARCHIVE):
        print("make_icons: no %s: the XBE goes without its dashboard pictures" % os.path.relpath(ARCHIVE, ROOT))
        return None
    icon = read_icon()
    if icon is None:
        print("make_icons: the Jiggy sprite isn't decoded yet: the XBE goes without its dashboard pictures")
        return None
    w, h, rows = icon
    os.makedirs(OUT, exist_ok=True)
    out = []
    for name, tex, what, size in (("titleimage", "TitleImage", "title image", 128),
                                  ("saveimage", "SaveImage", "save image", 64)):
        scale = size // w

        def pixel(x, y):
            r, g, b, a = rows[min(h - 1, y // scale)][min(w - 1, x // scale)]
            return tuple((c * a + gc * (255 - a)) // 255 for c, gc in zip((r, g, b), GROUND))

        write_bmp(os.path.join(OUT, name + ".bmp"), size, pixel)
        with open(os.path.join(OUT, name + ".rdf"), "w") as f:
            f.write(RDF % dict(name=name, tex=tex, what=what, size=size))
        r = subprocess.run([os.path.join(RXDK, "tools", "bundler.exe"), "-q", name + ".rdf"], cwd=OUT,
                           capture_output=True, text=True)
        xbx = os.path.join(OUT, name + ".xbx")
        if r.returncode != 0 or not os.path.isfile(xbx):
            sys.stdout.write(r.stdout + r.stderr)
            raise SystemExit("make_icons: bundler failed on %s.rdf" % name)
        out.append(xbx)
    print("make_icons: %s (%d bytes), %s (%d bytes)" % (
        os.path.relpath(out[0], ROOT), os.path.getsize(out[0]), os.path.relpath(out[1], ROOT),
        os.path.getsize(out[1])))
    return out


if __name__ == "__main__":
    main()
