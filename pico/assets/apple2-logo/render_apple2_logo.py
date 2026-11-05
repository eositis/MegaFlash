#!/usr/bin/env python3
"""Render MegaFlash logo for Apple IIc Hi-Res / Double Hi-Res / Double Low-Res.

Requires: Pillow  (pip install Pillow)

Usage:
  python3 render_apple2_logo.py [source.png]

Defaults to megaflash_logo_source.png next to this script.
Writes native-resolution PNGs, preview sheets, palette index maps,
packed DLGR bytes, and an approximate HGR screen dump (8192 bytes @ $2000).
"""

from __future__ import annotations

import os
import sys

from PIL import Image, ImageDraw, ImageEnhance

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SRC = os.path.join(HERE, "megaflash_logo_source.png")
OUT = HERE

# Apple II Hi-Res artifact palette (6 colors)
HGR_PALETTE = [
    (0x00, 0x00, 0x00),  # Black
    (0xFF, 0xFF, 0xFF),  # White
    (0x2F, 0xCC, 0x1A),  # Green
    (0xD0, 0x43, 0xE5),  # Violet
    (0xD0, 0x6A, 0x1A),  # Orange
    (0x2F, 0x95, 0xE5),  # Blue
]

# Apple IIe/IIc 16-color palette (DHGR / DLGR) — common RGB approximations
A2_16 = [
    (0x00, 0x00, 0x00),  # 0 Black
    (0x90, 0x17, 0x40),  # 1 Magenta
    (0x40, 0x2C, 0xA5),  # 2 Dark Blue
    (0xD0, 0x43, 0xE5),  # 3 Purple
    (0x00, 0x69, 0x40),  # 4 Dark Green
    (0x80, 0x80, 0x80),  # 5 Grey 1
    (0x2F, 0x95, 0xE5),  # 6 Medium Blue
    (0xBF, 0xAB, 0xFF),  # 7 Light Blue
    (0x40, 0x54, 0x00),  # 8 Brown
    (0xD0, 0x6A, 0x1A),  # 9 Orange
    (0xA0, 0xA0, 0xA0),  # 10 Grey 2
    (0xFF, 0x96, 0xBF),  # 11 Pink
    (0x2F, 0xCC, 0x1A),  # 12 Light Green
    (0xD0, 0xD3, 0x5A),  # 13 Yellow
    (0x6A, 0xEE, 0xBF),  # 14 Aquamarine
    (0xFF, 0xFF, 0xFF),  # 15 White
]

NAMES_16 = [
    "Black", "Magenta", "Dark Blue", "Purple",
    "Dark Green", "Grey 1", "Medium Blue", "Light Blue",
    "Brown", "Orange", "Grey 2", "Pink",
    "Light Green", "Yellow", "Aquamarine", "White",
]


def dist2(a, b):
    return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2


def nearest(rgb, palette):
    best_i, best_d = 0, 1e18
    for i, p in enumerate(palette):
        d = dist2(rgb, p)
        if d < best_d:
            best_d, best_i = d, i
    return best_i


def prepare_logo(src_path: str) -> Image.Image:
    im = Image.open(src_path).convert("RGBA")
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 16 or (r > 230 and g > 230 and b > 230 and min(r, g, b) > 220):
                px[x, y] = (0, 0, 0, 0)
    bbox = im.getbbox()
    if bbox:
        im = im.crop(bbox)
    rgb = ImageEnhance.Contrast(im.convert("RGB")).enhance(1.15)
    rgb = ImageEnhance.Color(rgb).enhance(1.2)
    out = Image.new("RGBA", im.size)
    out.paste(rgb)
    out.putalpha(im.split()[-1])
    return out


def fit_on_canvas(logo_rgba, tw, th, bg=(0, 0, 0), margin_frac=0.04, resample=Image.Resampling.LANCZOS):
    canvas = Image.new("RGBA", (tw, th), bg + (255,))
    mw = int(tw * (1 - 2 * margin_frac))
    mh = int(th * (1 - 2 * margin_frac))
    lw, lh = logo_rgba.size
    scale = min(mw / lw, mh / lh)
    nw = max(1, int(round(lw * scale)))
    nh = max(1, int(round(lh * scale)))
    resized = logo_rgba.resize((nw, nh), resample)
    x = (tw - nw) // 2
    y = (th - nh) // 2
    canvas.alpha_composite(resized, (x, y))
    return canvas.convert("RGB")


def quantize_palette(img_rgb, palette, dither=False, dither_strength=5.0):
    w, h = img_rgb.size
    px = img_rgb.load()
    out = Image.new("RGB", (w, h))
    opx = out.load()
    bayer = [
        [0, 8, 2, 10],
        [12, 4, 14, 6],
        [3, 11, 1, 9],
        [15, 7, 13, 5],
    ]
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if dither:
                t = (bayer[y & 3][x & 3] - 7.5) * dither_strength
                r = max(0, min(255, int(r + t)))
                g = max(0, min(255, int(g + t)))
                b = max(0, min(255, int(b + t)))
            opx[x, y] = palette[nearest((r, g, b), palette)]
    return out


def hgr_artifact_quantize(img_rgb):
    w, h = img_rgb.size
    px = img_rgb.load()
    out = Image.new("RGB", (w, h))
    opx = out.load()
    set_gv = [(0, 0, 0), (0x2F, 0xCC, 0x1A), (0xD0, 0x43, 0xE5), (255, 255, 255)]
    set_ob = [(0, 0, 0), (0xD0, 0x6A, 0x1A), (0x2F, 0x95, 0xE5), (255, 255, 255)]
    for y in range(h):
        x = 0
        while x < w:
            group_end = min(x + 7, w)
            err_gv = err_ob = 0
            samples = []
            for gx in range(x, group_end):
                rgb = px[gx, y]
                samples.append(rgb)
                err_gv += min(dist2(rgb, c) for c in set_gv)
                err_ob += min(dist2(rgb, c) for c in set_ob)
            use = set_gv if err_gv <= err_ob else set_ob
            for i, gx in enumerate(range(x, group_end)):
                opx[gx, y] = min(use, key=lambda c: dist2(samples[i], c))
            x = group_end
    return out


def scale_preview(img, factor):
    return img.resize((img.width * factor, img.height * factor), Image.Resampling.NEAREST)


def make_palette_strip(palette, cell=28):
    n = len(palette)
    strip = Image.new("RGB", (n * cell, cell + 18), (30, 30, 30))
    d = ImageDraw.Draw(strip)
    for i, c in enumerate(palette):
        d.rectangle([i * cell, 0, (i + 1) * cell - 1, cell - 1], fill=c)
        d.text((i * cell + 4, cell + 2), str(i), fill=(200, 200, 200))
    return strip


def compose_labeled(img, title, subtitle, palette, scale=3):
    preview = scale_preview(img, scale)
    strip = make_palette_strip(palette)
    pad = 16
    header = 48
    w = max(preview.width, strip.width) + pad * 2
    h = header + preview.height + strip.height + pad * 3
    canvas = Image.new("RGB", (w, h), (24, 24, 28))
    d = ImageDraw.Draw(canvas)
    d.text((pad, 12), title, fill=(255, 220, 120))
    d.text((pad, 30), subtitle, fill=(180, 180, 190))
    canvas.paste(preview, (pad, header))
    canvas.paste(strip, (pad, header + preview.height + pad))
    return canvas


def hgr_addr(y: int) -> int:
    return ((y & 0x07) << 10) | ((y & 0x38) << 4) | ((y & 0xC0) >> 2)


def encode_hgr(img_280x192: Image.Image) -> bytes:
    assert img_280x192.size == (280, 192)
    px = img_280x192.load()
    mem = bytearray(8192)
    set_gv = [(0, 0, 0), (0x2F, 0xCC, 0x1A), (0xD0, 0x43, 0xE5), (255, 255, 255)]
    set_ob = [(0, 0, 0), (0xD0, 0x6A, 0x1A), (0x2F, 0x95, 0xE5), (255, 255, 255)]
    for y in range(192):
        base = hgr_addr(y)
        for byte_i in range(40):
            x0 = byte_i * 7
            err_gv = err_ob = 0
            for dx in range(7):
                x = x0 + dx
                if x >= 280:
                    break
                rgb = px[x, y]
                err_gv += min(dist2(rgb, c) for c in set_gv)
                err_ob += min(dist2(rgb, c) for c in set_ob)
            high = 0x80 if err_ob < err_gv else 0x00
            use = set_ob if high else set_gv
            bits = 0
            for dx in range(7):
                x = x0 + dx
                if x >= 280:
                    break
                c = min(use, key=lambda c: dist2(px[x, y], c))
                on = 0 if c == (0, 0, 0) else 1
                bits |= on << dx
            mem[base + byte_i] = high | bits
    return bytes(mem)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    if not os.path.isfile(src):
        print(f"Source not found: {src}", file=sys.stderr)
        sys.exit(1)

    logo = prepare_logo(src)
    print(f"keyed logo: {logo.size} from {src}")

    # Double Low-Res 80×48
    dlgr_src = fit_on_canvas(logo, 80, 48, margin_frac=0.02, resample=Image.Resampling.BOX)
    dlgr_src = ImageEnhance.Sharpness(dlgr_src).enhance(1.4)
    dlgr = quantize_palette(dlgr_src, A2_16, dither=True, dither_strength=3.5)
    dlgr.save(os.path.join(OUT, "megaflash_dlgr_80x48.png"))
    compose_labeled(
        dlgr, "MEGAFLASH — Double Low-Res (DLGR)",
        "80×48 · 16 colors · Apple IIc", A2_16, scale=10,
    ).save(os.path.join(OUT, "megaflash_dlgr_preview.png"))

    idx = bytearray(80 * 48)
    px = dlgr.load()
    for y in range(48):
        for x in range(80):
            idx[y * 80 + x] = nearest(px[x, y], A2_16)
    open(os.path.join(OUT, "megaflash_dlgr_80x48.idx"), "wb").write(idx)
    packed = bytearray()
    for y in range(48):
        for x in range(0, 80, 2):
            lo = idx[y * 80 + x] & 0x0F
            hi = idx[y * 80 + x + 1] & 0x0F
            packed.append(lo | (hi << 4))
    open(os.path.join(OUT, "megaflash_dlgr_packed.bin"), "wb").write(packed)

    # Hi-Res 280×192
    hgr_src = fit_on_canvas(logo, 280, 192, margin_frac=0.04)
    hgr_src = ImageEnhance.Sharpness(hgr_src).enhance(1.25)
    hgr = hgr_artifact_quantize(hgr_src)
    hgr.save(os.path.join(OUT, "megaflash_hgr_280x192.png"))
    compose_labeled(
        hgr, "MEGAFLASH — Hi-Res (HGR)",
        "280×192 · 6 artifact colors (Green/Violet · Orange/Blue) · Apple IIc",
        HGR_PALETTE, scale=3,
    ).save(os.path.join(OUT, "megaflash_hgr_preview.png"))
    open(os.path.join(OUT, "megaflash_hgr.bin"), "wb").write(encode_hgr(hgr))

    # Double Hi-Res (140 color cells → 560×192 display)
    dhgr_hi = fit_on_canvas(logo, 560, 192, margin_frac=0.04)
    dhgr_cells = ImageEnhance.Sharpness(
        dhgr_hi.resize((140, 192), Image.Resampling.LANCZOS)
    ).enhance(1.3)
    dhgr_cells_q = quantize_palette(dhgr_cells, A2_16, dither=True, dither_strength=4.0)
    dhgr = dhgr_cells_q.resize((560, 192), Image.Resampling.NEAREST)
    dhgr.save(os.path.join(OUT, "megaflash_dhgr_560x192.png"))
    dhgr_cells_q.save(os.path.join(OUT, "megaflash_dhgr_cells_140x192.png"))
    compose_labeled(
        dhgr, "MEGAFLASH — Double Hi-Res (DHGR)",
        "560×192 (140 color cells ×192) · 16 colors · Apple IIc",
        A2_16, scale=2,
    ).save(os.path.join(OUT, "megaflash_dhgr_preview.png"))

    idx2 = bytearray(140 * 192)
    px2 = dhgr_cells_q.load()
    for y in range(192):
        for x in range(140):
            idx2[y * 140 + x] = nearest(px2[x, y], A2_16)
    open(os.path.join(OUT, "megaflash_dhgr_140x192.idx"), "wb").write(idx2)

    previews = [
        ("Double Low-Res 80×48", scale_preview(dlgr, 8)),
        ("Hi-Res 280×192", scale_preview(hgr, 2)),
        ("Double Hi-Res 560×192", scale_preview(dhgr, 2)),
    ]
    max_h = max(p.height for _, p in previews)
    pad, titles_h = 12, 28
    total_w = sum(p.width for _, p in previews) + pad * (len(previews) + 1)
    total_h = max_h + titles_h + pad * 2 + 40
    sheet = Image.new("RGB", (total_w, total_h), (20, 20, 24))
    d = ImageDraw.Draw(sheet)
    d.text((pad, 8), "MEGAFLASH logo — Apple IIc graphics modes", fill=(255, 210, 100))
    x = pad
    for title, p in previews:
        d.text((x, titles_h - 4), title, fill=(200, 200, 210))
        sheet.paste(p, (x, titles_h + pad + 8))
        x += p.width + pad
    sheet.save(os.path.join(OUT, "megaflash_apple2_modes_sheet.png"))

    card = Image.new("RGB", (640, 220), (24, 24, 28))
    d = ImageDraw.Draw(card)
    d.text((16, 12), "Apple IIc 16-color palette (DHGR / DLGR)", fill=(255, 220, 120))
    cw = 38
    for i, (c, name) in enumerate(zip(A2_16, NAMES_16)):
        x = 16 + (i % 8) * (cw + 40)
        y = 44 + (i // 8) * 80
        d.rectangle([x, y, x + cw, y + cw], fill=c, outline=(80, 80, 80))
        d.text((x + cw + 6, y + 8), f"{i}", fill=(230, 230, 230))
        d.text((x + cw + 6, y + 24), name, fill=(160, 160, 170))
    d.text((16, 200), "HGR 6-color: Black, White, Green, Violet, Orange, Blue", fill=(180, 180, 190))
    card.save(os.path.join(OUT, "apple2_palette_reference.png"))

    print("Wrote assets to", OUT)


if __name__ == "__main__":
    main()
