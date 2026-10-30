#!/usr/bin/env python3
"""ChronoStretch's script logo (art/logo.png, 384 x 62, used by art/make_art.py): "Chrono" in light steel and "Stretch" in
ice blue, a script face, a soft shadow. Run once with a script font's path; the PNG is kept in the repo, the font is not.
  python3 art/make_logo.py <font.ttf>     (0.7 used Segoe Script Bold, segoescb.ttf, from a Windows install)"""
import os
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 384, 62
LIGHT, ICE = (233, 238, 242, 255), (150, 214, 246, 255)


def render(path, scale=4):
    w, h = W * scale, H * scale
    size = 60 * scale
    while True:   # the largest size that fits the box
        f = ImageFont.truetype(path, size)
        a, b = "Chrono", "Stretch"
        wa, wb = f.getlength(a), f.getlength(b)
        box = f.getbbox(a + b)
        if wa + wb <= w - 8 * scale and box[3] - box[1] <= h - 4 * scale:
            break
        size -= scale
    top = (h - (box[3] - box[1])) // 2 - box[1]
    x0 = (w - (wa + wb)) // 2
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    sh = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ds = ImageDraw.Draw(sh)
    ds.text((x0 + 2 * scale, top + 3 * scale), a, font=f, fill=(0, 0, 0, 200))
    ds.text((x0 + wa + 2 * scale, top + 3 * scale), b, font=f, fill=(0, 0, 0, 200))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(3 * scale)))
    d = ImageDraw.Draw(img)
    d.text((x0, top), a, font=f, fill=LIGHT)
    d.text((x0 + wa, top), b, font=f, fill=ICE)
    return img.resize((W, H), Image.LANCZOS)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    render(sys.argv[1]).save(os.path.join(HERE, "logo.png"), optimize=True)
    print("make_logo: art/logo.png")
