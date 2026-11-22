#!/usr/bin/env python3
"""An HD texture pack made for Lighthouse on a PC, made into one the Xbox can hold.

    python tools/hd_pack.py packs/bk-reloaded-v0.2.1.o2r
    python tools/hd_pack.py PACK [--out data/mods/NAME-xbox.o2r] [--scale 2]

setup.py runs it for every pack in packs/ (setup.py --hd-packs downloads BKHD Loulof and BK Reloaded there); run it
yourself to make one with other settings.

A Lighthouse pack (BK Reloaded) is libultraship textures under alt/ (alt/assets/x/y for the game's assets/x/y), at 2 to
16 times the game's size and 32 bits a texel, which Lighthouse draws instead of the game's while its alternate assets
are on. On the Xbox that is far more than it can hold (BK Reloaded v0.2.1, the interface only: 76 MB), so, here, on
the PC (as Mario-Kart-64XX's tools/hd_pack.py):

  - each is made at most `--scale` (2) times the game's size, on each side, and no more than the Xbox's largest
    texture (512); one that can't be 2x is made 1x: the pack's own art at the game's size;
  - 16 bits a texel where the game's own texture couldn't say more (RGBA16, colour-indexed, IA4: one bit of alpha),
    grey ones as IA16, the rest RGBA32; the bytes scaled as libultraship's HD textures are (HByteScale, VPixelScale:
    a load of part of a texture still finds its part), read by libultraship's ImportTextureRaw as they are. (The
    bold font's masks are RGBA32 in the game and its spheres RGBA16, so they stay so: Lighthouse's AltBoldFont
    composites from them reading those two formats only.)

The result goes in data/mods/ (git-ignored), and from there onto the disc (tools/build.py), into mods/ beside
default.xbe, which the game reads at start (xbox/src/port/lhx_nomods.cpp). The pack itself isn't kept there: put it
somewhere build.py won't copy it, such as packs/.

Needs Pillow (pip install pillow).
"""
import argparse
import os
import struct
import sys
import zipfile

try:
    from PIL import Image, ImageChops, ImageFilter, ImageMath
except ImportError:
    raise SystemExit("hd_pack: needs Pillow: pip install pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAX_SIZE = 512  # the Xbox's largest texture (gfx_direct3d8.cpp GetMaxTextureSize)

# libultraship's TextureType (fast/resource/type/Texture.h): bytes a texel, and the alpha it can have
RGBA32, RGBA16, CI4, CI8, I4, I8, IA4, IA8, IA16 = 1, 2, 3, 4, 5, 6, 7, 8, 9
TEXEL_BYTES = {RGBA32: 4, RGBA16: 2, CI4: 0.5, CI8: 1, I4: 0.5, I8: 1, IA4: 0.5, IA8: 1, IA16: 2}
ONE_BIT_ALPHA = {RGBA16, CI4, CI8, IA4}  # (a colour index's palette is RGBA16)
GREY = {I4, I8, IA4, IA8, IA16}
TEX_FLAG_LOAD_AS_RAW = 1
ALT = "alt/"
JINJO_COLOURS = {"BLUE", "GREEN", "ORANGE", "PURPLE", "YELLOW"}  # AltSprites.cpp's kJinjoColors


def texture(b):
    """A texture resource's header and body: (header, type, width, height, flags, hscale, vscale, data), or None."""
    if len(b) < 0x50 or b[4:8] != b"XETO":
        return None
    version = struct.unpack_from("<I", b, 8)[0]
    if version == 0:
        ttype, w, h, n = struct.unpack_from("<IIII", b, 0x40)
        return b[:0x40], ttype, w, h, 0, 1.0, 1.0, b[0x50:0x50 + n]
    ttype, w, h, flags, hs, vs, n = struct.unpack_from("<IIIIffI", b, 0x40)
    return b[:0x40], ttype, w, h, flags, hs, vs, b[0x5C:0x5C + n]


def resource_texture(header, ttype, width, height, data, hscale, vscale):
    """A texture resource as version 1, with the HD scale (none: the game's size and type)."""
    head = bytearray(header[:0x40])
    struct.pack_into("<I", head, 8, 1)
    flags = 0 if hscale == 1 and vscale == 1 else TEX_FLAG_LOAD_AS_RAW
    return bytes(head) + struct.pack("<IIIIffI", ttype, width, height, flags, hscale, vscale, len(data)) + data


def shrink(img, w, h):
    """To w x h, through premultiplied alpha (a transparent texel's colour doesn't bleed into its neighbours)."""
    if img.size == (w, h):
        return img
    exact = img.width % w == 0 and img.height % h == 0
    method = Image.Resampling.BOX if exact else Image.Resampling.LANCZOS
    return img.convert("RGBa").resize((w, h), method).convert("RGBA")


def is_grey(img):
    r, g, b, _ = img.split()
    loose = lambda d: d.point(lambda v: 255 if v > 3 else 0).getbbox() is None
    return loose(ImageChops.difference(r, g)) and loose(ImageChops.difference(g, b))


def one_bit(img):
    """Alpha made 0 or 255, and the transparent texels given their neighbours' colour, for filtering at the edges."""
    a = img.getchannel("A").point(lambda v: 255 if v >= 128 else 0)
    around = img.convert("RGBa").filter(ImageFilter.BoxBlur(2)).convert("RGBA")
    out = Image.composite(img, around, a)
    out.putalpha(a)
    return out


def rgba16(img):
    """N64 RGBA16: 5-5-5-1, big-endian, as libultraship's own RGBA16 textures."""
    r, g, b, a = [c.convert("I") for c in img.split()]
    v = ImageMath.lambda_eval(
        lambda x: ((x["r"] >> 3) << 11) | ((x["g"] >> 3) << 6) | ((x["b"] >> 3) << 1) | (x["a"] >> 7),
        r=r, g=g, b=b, a=a)
    return v.tobytes("raw", "I;16B")


def convert(pack_path, base_path, out_path, scale, notes):
    pack = zipfile.ZipFile(pack_path)
    base = zipfile.ZipFile(base_path)
    base_names = set(base.namelist())
    out = zipfile.ZipFile(out_path + ".part", "w", zipfile.ZIP_DEFLATED, compresslevel=6)

    stats = {}
    skipped = []
    before = 0
    names = [n for n in pack.namelist() if n.startswith(ALT) and not n.endswith("/")]
    for count, name in enumerate(names, 1):
        path = name[len(ALT):]
        folder = "/".join(path.split("/")[:2])
        # a map's bold letters aren't one of the game's textures: Lighthouse makes each (glyph mask x sphere) at the
        # mask's size, 32 bits a texel (port/Resource/Alt/AltBoldFont.cpp: assets/boldfont/<mask>_<sphere id>)
        game_path = path
        if path.startswith("assets/boldfont/"):
            game_path = "assets/sprite/" + path.split("/")[-1].rsplit("_", 1)[0]
        elif "_frame_" in path and path not in base_names:
            # an animated texture's later frames (<model>_tex_N_frame_F: port/Resource/Alt/AltAnimTextures.cpp) aren't
            # assets of the game's, but are its first frame's size and type; left out, the texture went from HD to
            # the game's and back as it animated
            game_path = path.rsplit("_frame_", 1)[0]
        elif path.rsplit("_", 1)[-1] in JINJO_COLOURS and path not in base_names:
            # the pause screen's Jinjo heads: one sprite the game colours by palette, a texture of its own a colour
            # in a pack (<sprite>_BLUE: port/Resource/Alt/AltSprites.cpp); left out, every head was the one colour
            game_path = path.rsplit("_", 1)[0]
        if game_path not in base_names:
            skipped.append((name, "not one of this game's assets (another version's or language's)"))
            continue
        hd = texture(pack.read(name))
        game = texture(base.read(game_path))
        if hd is None or game is None:
            skipped.append((name, "not a texture, or the game's isn't"))
            continue
        _, htype, hw, hh, _, _, _, hdata = hd
        header, otype, ow, oh = game[:4]
        if path.startswith("assets/boldfont/"):
            # (the letter Lighthouse makes is as wide as its mask rounded up to 4 texels, a TMEM line: 6 -> 8, and the
            # pack's art is that times its scale, 64 for 8; made from the mask's 6, it was read with the wrong rows)
            header, otype = hd[0], RGBA32
            if oh > 0 and hh % oh == 0 and hw % (hh // oh) == 0:
                ow = hw // (hh // oh)
        if htype != RGBA32 or otype not in TEXEL_BYTES or ow == 0 or oh == 0 or len(hdata) < hw * hh * 4:
            skipped.append((name, "a texture type it doesn't know"))
            continue
        before += hw * hh * 4
        # (not always the same on both sides: the dialog font's 8x13 glyphs are 64x96; the game draws the art over its
        # own glyph whatever its shape, so it's made 2x the game's on both)
        sx, sy = hw / ow, hh / oh
        if sx < 1 or sy < 1:
            skipped.append((name, "%dx%d is smaller than the game's %dx%d" % (hw, hh, ow, oh)))
            continue
        img = Image.frombytes("RGBA", (hw, hh), bytes(hdata[:hw * hh * 4]))
        s = scale if max(ow, oh) * scale <= MAX_SIZE and min(sx, sy) >= scale else 1
        w, h = ow * s, oh * s
        k = hw // ow
        if hw == ow * k and hh % k == 0 and hh // k != oh:
            # Its art is k times a glyph of another ROM revision's height (the dialog font: 8x12 on US 1.0, 8x13
            # from 1.1 on, the row added at the top): made s times its own rows and set at the bottom, the top padded
            # or cut, as Lighthouse's AltDialogFont.cpp corrects a 32-bit pack (not stretched to the rows)
            own = shrink(img, w, (hh // k) * s)
            img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
            img.paste(own, (0, h - own.height))
        else:
            img = shrink(img, w, h)

        if otype in GREY and is_grey(img):
            ttype, data = IA16, Image.merge("LA", (img.getchannel("R"), img.getchannel("A"))).tobytes()
        elif otype in ONE_BIT_ALPHA:
            ttype, data = RGBA16, rgba16(one_bit(img))
        else:
            ttype, data = RGBA32, img.tobytes()
        if s == 1 and ttype == otype:
            hscale = vscale = 1.0
        else:
            hscale = (TEXEL_BYTES[ttype] * w) / (TEXEL_BYTES[otype] * ow)
            vscale = float(s)
        out.writestr(name, resource_texture(header, ttype, w, h, data, hscale, vscale))
        st = stats.setdefault(folder, [0, 0])
        st[0] += 1
        st[1] += len(data)
        if count % 200 == 0:
            print("hd_pack: %d of %d" % (count, len(names)), flush=True)

    for note in notes:  # the pack's own notes (its authors)
        out.write(note, os.path.basename(note))
    out.close()
    os.replace(out_path + ".part", out_path)

    print("\nhd_pack: %s" % out_path)
    total = 0
    for folder, (n, size) in sorted(stats.items()):
        total += size
        print("  %-32s %5d textures %8.1f MB" % (folder, n, size / 1e6))
    print("  all its textures, as they are held in memory: %.1f MB (the pack's: %.1f MB; the file: %.1f MB)"
          % (total / 1e6, before / 1e6, os.path.getsize(out_path) / 1e6))
    if skipped:
        print("  left out: %d" % len(skipped))
        why = {}
        for name, reason in skipped:
            why.setdefault(reason, []).append(name)
        for reason, which in sorted(why.items(), key=lambda kv: -len(kv[1])):
            print("    %d: %s (e.g. %s)" % (len(which), reason, which[0]))
            if os.environ.get("HD_PACK_LIST"):
                for name in which:
                    print("      " + name)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pack", help="the PC pack (.o2r)")
    ap.add_argument("--base", default=os.path.join(ROOT, "data", "bk.o2r"), help="the game's own (data/bk.o2r)")
    ap.add_argument("--out", help="where it goes (data/mods/<pack>-xbox.o2r)")
    ap.add_argument("--scale", type=int, default=2, help="how many times the game's size at most (2)")
    args = ap.parse_args()
    if not os.path.isfile(args.base):
        raise SystemExit("hd_pack: no %s: run setup.py first" % args.base)
    stem = os.path.splitext(os.path.basename(args.pack))[0]
    out = args.out or os.path.join(ROOT, "data", "mods", stem + "-xbox.o2r")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    notes = [p for p in (os.path.splitext(args.pack)[0] + ".txt",) if os.path.isfile(p)]
    convert(args.pack, args.base, out, max(1, args.scale), notes)


if __name__ == "__main__":
    sys.exit(main())
