#!/usr/bin/env python3
"""Generates the PocketBook launcher icons (8-bit BMP) for Better Stats.
Minimal: an open book + rising bars. Black on white; the _f variant is
inverted (the firmware's focused style)."""
import os
import sys
from PIL import Image, ImageDraw

W, H = 106, 128
SS = 4  # supersampling for smooth edges


def draw_icon(fg, bg):
    im = Image.new("L", (W * SS, H * SS), bg)
    d = ImageDraw.Draw(im)
    s = SS

    def R(x0, y0, x1, y1, **kw):
        d.rectangle([x0 * s, y0 * s, x1 * s, y1 * s], **kw)

    # --- rising bars (stats) in the upper area ---
    bar_w = 16
    gap = 6
    xs = 20
    heights = [22, 34, 48]          # rising
    base_y = 66
    for i, bh in enumerate(heights):
        x = xs + i * (bar_w + gap)
        R(x, base_y - bh, x + bar_w, base_y, fill=fg)

    # --- open book at the bottom ---
    # book spine center
    cx = W // 2
    top = 74          # top edge of the pages (slight curve)
    bot = 104         # bottom edge
    left = 12
    right = W - 12
    lw = 4 * s        # line width

    # two pages as outlines: trapezoids, rising toward the spine in the middle
    # left page
    d.line([(left * s, (top + 6) * s), (cx * s, top * s)], fill=fg, width=lw)
    d.line([(left * s, (top + 6) * s), (left * s, bot * s)], fill=fg, width=lw)
    d.line([(left * s, bot * s), (cx * s, (bot - 6) * s)], fill=fg, width=lw)
    # right page
    d.line([(right * s, (top + 6) * s), (cx * s, top * s)], fill=fg, width=lw)
    d.line([(right * s, (top + 6) * s), (right * s, bot * s)], fill=fg, width=lw)
    d.line([(right * s, bot * s), (cx * s, (bot - 6) * s)], fill=fg, width=lw)
    # spine
    d.line([(cx * s, top * s), (cx * s, (bot - 6) * s)], fill=fg, width=lw)

    im = im.resize((W, H), Image.LANCZOS)
    # quantize to 8-bit grayscale with a fixed palette
    return im.convert("P", palette=Image.ADAPTIVE, colors=16)


def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    normal = draw_icon(fg=0, bg=255)      # black on white
    focused = draw_icon(fg=255, bg=0)     # inverted
    normal.save(os.path.join(outdir, "betterstats.bmp"))
    focused.save(os.path.join(outdir, "betterstats_f.bmp"))
    print("written:", outdir)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
