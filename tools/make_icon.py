#!/usr/bin/env python3
"""Generates the PocketBook launcher icons (8-bit BMP) for Better Stats.
Minimal: an open book + rising bars. The plain variant is black on white; the
_f variant is the firmware's focused style, a filled black disc with the glyph
knocked out -- a disc, not the full canvas, so it matches the round highlight
the stock apps use."""
import os
import sys
from PIL import Image, ImageDraw

W, H = 106, 128
SS = 4  # supersampling for smooth edges

CX, CY = W // 2, H // 2
RADIUS = 51                     # focus disc: full canvas width, minus a hair


def draw_icon(focused):
    """focused=False: black glyph on white. focused=True: white glyph on a
    black disc, canvas corners left white."""
    im = Image.new("L", (W * SS, H * SS), 255)
    d = ImageDraw.Draw(im)
    s = SS
    fg = 0

    if focused:
        d.ellipse([(CX - RADIUS) * s, (CY - RADIUS) * s,
                   (CX + RADIUS) * s, (CY + RADIUS) * s], fill=0)
        fg = 255

    def R(x0, y0, x1, y1, **kw):
        d.rectangle([x0 * s, y0 * s, x1 * s, y1 * s], **kw)

    def poly(points):
        d.line([(x * s, y * s) for x, y in points],
               fill=fg, width=4 * s, joint="curve")

    # Glyph sits inside the disc: bars above, book below, the whole stack
    # centred on CY. Every corner stays ~7px clear of RADIUS.
    dy = 3

    # --- rising bars (stats) ---
    bar_w, gap = 12, 5
    heights = [14, 22, 30]
    base_y = 58 + dy
    xs = CX - (3 * bar_w + 2 * gap) // 2
    for i, bh in enumerate(heights):
        x = xs + i * (bar_w + gap)
        R(x, base_y - bh, x + bar_w, base_y, fill=fg)

    # --- open book, pages sloping *down* to the spine so it reads as open
    # towards the viewer rather than tented over ---
    left, right = 18, W - 18
    outer_top, spine_top = 66 + dy, 74 + dy
    outer_bot, spine_bot = 86 + dy, 94 + dy
    poly([(left, outer_top), (CX, spine_top), (CX, spine_bot),
          (left, outer_bot), (left, outer_top)])
    poly([(right, outer_top), (CX, spine_top), (CX, spine_bot),
          (right, outer_bot), (right, outer_top)])

    im = im.resize((W, H), Image.LANCZOS)
    return im.convert("P", palette=Image.ADAPTIVE, colors=16)


def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    draw_icon(focused=False).save(os.path.join(outdir, "betterstats.bmp"))
    draw_icon(focused=True).save(os.path.join(outdir, "betterstats_f.bmp"))
    print("written:", outdir)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
