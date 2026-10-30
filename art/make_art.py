#!/usr/bin/env python3
"""ChronoStretch 0.7's skin: draws the dark brushed-steel faceplates (ice-blue accents), the knob, button, segment and toggle looks, the screen's
filmstrips and pictures (Pillow, PNGs in art/), the browser tile and the stylesheet, and writes layout.conf and
art/geometry.json, so the boxes, the layout and skin_post.py always agree. Edit this script, not its outputs.
Run from ports/chrono:  python3 art/make_art.py  (tools/gen.ps1 runs it).

MAIN (layout coordinates; the plugin area is y 86..714), Serato-style:
  a top bar (the sound, CHOP, the bank); a smoked-glass screen with the sound's tempo and key, the view of the sound as a
  waveform (wv000..wv287: display-only filmstrip meters, 4 px each, 25 heights x 3 colours: bass-heavy red, mid green,
  bright blue), over each column an overlay meter (ov000..ov287: the played part, the playhead in the pad's colour, the
  chop lines in their pads' colours, the other bank's grey), and under it the tab row (mk00..mk35: 56 px meters 32 px
  apart, so a tab drawn at its column's offset in the slot puts its point on its line; the selected chop's tab lit); a
  drag strip over each tab slot (td00..td35) grabs the tab there and moves its chop; the
  4x4 pads, bottom-left first like MPC's (lp00..lp15: each pad its own colour, lit while it plays; the option grid
  pad_tap over them selects, the selected cell ringed); Chop Mode, its setting (Sensitivity, Cues or Chop Every) and
  Zoom; Scroll, Key (its readout between - and +), Pitch, Volume and the selected pad's Level; Sync, Play Mode, Drums.
PAD: the selected pad's settings. SOUND: time, key, layers, chops, playing. BROWSE: the drives' folders (a scroll strip and
its bar beside the rows), LOAD. The logo is art/logo.png (art/make_logo.py, a script face)."""
import colorsys
import json
import math
import os
import random
import re
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
random.seed(5)
Y0 = 86                                  # the plugin area's top (layout y); the backdrops are 1280 x 628
W, H = 1280, 628
NCOL, COL_W = 288, 4
WV = (64, 192, NCOL * COL_W, 160)        # x, y, w, h of the waveform
WAVE_FRAMES, OV_FRAMES = 76, 68
NMARK, MK_STEP, MK_W, MK_H = 36, 32, 56, 28
COLS_PER_MARK = MK_STEP // COL_W
MK_FRAMES = 1 + 16 * COLS_PER_MARK * 2
MK_Y = WV[1] + WV[3] + 2
MN_H, MN_DY, MN_FRAMES = 16, 4, 1 + 64 * COLS_PER_MARK   # the tab numbers: a 16 px meter MN_DY under the tab row's top
GRID_CX, GRID_FIRST_CY, CELL_W, CELL_H, GAP = 270, 444, 118, 70, 2
LOGO = (14, 96, 286, 46)                 # the script logo (art/logo.png), top left
BR_HEAD_W = 688                          # BROWSE's: the highlighted file's name
HEAD_X, HEAD_W = 312, 460                # the sound's name on MAIN and BROWSE, right of the logo
BTN_W, BTN_H = 76, 36                    # every button that shows on two pages: one size (the builder bakes one image a button)
SOUND_HEAD_X, SOUND_HEAD_W = 312, 760    # and on SOUND
BROWSE_LIST = (36, 196, 1050, 512)       # the browser's rows (layout x, y, w, h)
SCREEN = (36, 150, 1208, 244)            # the smoked glass: x, y, w, h (layout)

WAVE_COLS = [(238, 78, 62), (92, 218, 118), (72, 162, 255)]   # bass-heavy, mid, bright
PLATE_TOP, PLATE_BOTTOM = (92, 96, 102), (56, 59, 64)          # dark brushed steel
POCKET_TOP, POCKET_BOTTOM = (70, 74, 80), (60, 63, 69)
ICE_HI, ICE_LO = (206, 236, 252), (126, 182, 214)              # ice-blue brushed steel (accents, knob caps, lit buttons)
STEEL_HI, STEEL_LO = (116, 121, 128), (72, 76, 82)             # unlit buttons


def pad_colour(s):   # 16 hues around the wheel, red first
    r, g, b = colorsys.hsv_to_rgb((s * 0.0625) % 1.0, 0.76, 0.98)
    return (int(r * 255), int(g * 255), int(b * 255))


PADS = [pad_colour(s) for s in range(16)]


def save(img, name):
    img.save(os.path.join(HERE, name), optimize=True)


def blank(w, h):
    return Image.new("RGBA", (w, h), (0, 0, 0, 0))


TITILLIUM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools", "html_art", "fonts", "TitilliumWeb-SemiBold.ttf")


def font(size, face=None):   # face: a TrueType file (the skin's own Titillium for button words), else PIL's default
    if face and os.path.exists(face):
        return ImageFont.truetype(face, size)
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def mix(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


# ---- textures ----
def streaks(w, h, coarse, fine):
    """Horizontal brushing: noise stretched along x (coarse and fine grain), mean 128."""
    a = Image.effect_noise((max(2, w // coarse), h), 64).resize((w, h), Image.BILINEAR)
    b = Image.effect_noise((max(2, w // fine), h), 64).resize((w, h), Image.BILINEAR)
    return ImageChops.blend(a, b, 0.45)


def brushed(w, h, top, bottom, grain=22, coarse=70, fine=9):
    """A brushed plate: a vertical gradient top..bottom, a soft diagonal sheen and the brushing."""
    grad = Image.new("RGB", (1, h))
    for y in range(h):
        grad.putpixel((0, y), mix(top, bottom, y / max(1, h - 1)))
    img = grad.resize((w, h))
    sheen = Image.new("L", (w, h))
    sd = ImageDraw.Draw(sheen)
    for x in range(-h, w, 3):   # a broad band of light across, top left to bottom right
        t = (x + h * 0.5) / (w + h)
        v = int(26 * math.exp(-((t - 0.32) / 0.16) ** 2) + 12 * math.exp(-((t - 0.78) / 0.1) ** 2))
        sd.line([(x, 0), (x + h, h)], fill=v, width=4)
    sheen = sheen.filter(ImageFilter.GaussianBlur(18))
    img = ImageChops.add(img, Image.merge("RGB", [sheen] * 3))
    s = streaks(w, h, coarse, fine).point(lambda v: max(0, min(255, int(128 + (v - 128) * grain / 64))))
    return ImageChops.add(img, Image.merge("RGB", [s] * 3), 1.0, -128)


def rounded_mask(w, h, r):
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, w - 1, h - 1], radius=r, fill=255)
    return m


def paste_rounded(dst, src, x, y, r):
    dst.paste(src, (x, y), rounded_mask(src.width, src.height, r))


def bevel(dst, box, r, light=(255, 255, 255, 120), dark=(0, 0, 0, 140), width=2, inset=False):
    """A raised (or sunk) edge: light along the top-left, dark along the bottom-right."""
    x0, y0, x1, y1 = box
    hi, lo = (dark, light) if inset else (light, dark)
    ov = blank(dst.width, dst.height)
    ImageDraw.Draw(ov).rounded_rectangle([x0, y0, x1, y1], radius=r, outline=lo, width=width)
    m = Image.new("L", ov.size, 0)
    ImageDraw.Draw(m).polygon([(x0, y0), (x1, y0), (x0, y1)], fill=255)
    top = blank(dst.width, dst.height)
    ImageDraw.Draw(top).rounded_rectangle([x0, y0, x1, y1], radius=r, outline=hi, width=width)
    ov.paste(top, (0, 0), m)
    dst.alpha_composite(ov)


def screw(dst, cx, cy, r=7):
    """A domed hex-socket screw, drawn at 4x."""
    s = 4 * (2 * r + 6)
    im = blank(s, s)
    d = ImageDraw.Draw(im)
    c = s // 2
    R = 4 * r
    d.ellipse([c - R - 6, c - R - 6, c + R + 6, c + R + 6], fill=(40, 42, 46, 200))
    for k in range(R, 0, -1):   # lit from the top left
        t = k / R
        v = int(120 + 110 * (1 - t) ** 0.6)
        o = 3 * (1 - t)
        d.ellipse([c - k - o, c - k - o, c + k - o, c + k - o], fill=(v, v + 2, v + 6, 255))
    hx = [(c + 0.42 * R * math.cos(math.pi / 3 * i + 0.3), c + 0.42 * R * math.sin(math.pi / 3 * i + 0.3)) for i in range(6)]
    d.polygon(hx, fill=(30, 31, 34, 255))
    im = im.resize((s // 4, s // 4), Image.LANCZOS)
    dst.alpha_composite(im, (int(cx - s // 8), int(cy - s // 8)))


def glass(w, h):
    """Smoked glass: near-black, a faint cool gradient, a soft reflection across the top."""
    g = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(g)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=mix((22, 25, 31), (7, 8, 11), t ** 0.7) + (255,))
    refl = blank(w, h)
    ImageDraw.Draw(refl).polygon([(0, 0), (w * 0.62, 0), (w * 0.42, h * 0.34), (0, h * 0.34)], fill=(255, 255, 255, 9))
    g.alpha_composite(refl.filter(ImageFilter.GaussianBlur(10)))
    return g


def well(dst, box, r=8, ice=False):
    """A sunk bezel round a glass or pad area: a lip (dark steel, or an ice-blue brushed accent ring), shadow at the top."""
    x0, y0, x1, y1 = box
    t = 5 if ice else 4
    lip = blank(dst.width, dst.height)
    if ice:
        ring = brushed(x1 - x0 + 2 * t + 1, y1 - y0 + 2 * t + 1, ICE_HI, ICE_LO, grain=26, coarse=40, fine=7).convert("RGBA")
        lip.paste(ring, (x0 - t, y0 - t), rounded_mask(ring.width, ring.height, r + t))
    else:
        ImageDraw.Draw(lip).rounded_rectangle([x0 - t, y0 - t, x1 + t, y1 + t], radius=r + t, fill=(30, 32, 36, 255))
    dst.alpha_composite(lip)
    bevel(dst, (x0 - t, y0 - t, x1 + t, y1 + t), r + t, light=(255, 255, 255, 110 if ice else 50), dark=(0, 0, 0, 150), width=2, inset=not ice)


def faceplate():
    img = brushed(W, H, PLATE_TOP, PLATE_BOTTOM).convert("RGBA")
    bevel(img, (1, 1, W - 2, H - 2), 10, light=(255, 255, 255, 90), dark=(0, 0, 0, 200), width=2)
    for x, y in ((14, 14), (W - 14, 14), (14, H - 14), (W - 14, H - 14)):
        screw(img, x, y)
    return img


def ice_bar(img, box, r=2):   # an ice-blue brushed accent strip (layout box)
    x, y, w, h = box
    y -= Y0
    bar = brushed(w, h, ICE_HI, ICE_LO, grain=26, coarse=40, fine=7).convert("RGBA")
    paste_rounded(img, bar, x, y, r)
    bevel(img, (x, y, x + w - 1, y + h - 1), r, light=(255, 255, 255, 140), dark=(0, 0, 0, 120), width=1)


def glass_panel(img, box, r=10, ice=False):   # layout box -> a glass well on the faceplate
    x, y, w, h = box
    y -= Y0
    well(img, (x, y, x + w - 1, y + h - 1), r, ice)
    paste_rounded(img, glass(w, h), x, y, r)
    edge = blank(img.width, img.height)
    ImageDraw.Draw(edge).rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=r, outline=(0, 0, 0, 255), width=2)
    img.alpha_composite(edge)


def anodised_panel(img, box, r=10, colour=(22, 23, 26)):   # a dark inset plate (the pads), ringed in ice blue
    x, y, w, h = box
    y -= Y0
    well(img, (x, y, x + w - 1, y + h - 1), r, ice=True)
    plate_ = brushed(w, h, mix(colour, (255, 255, 255), 0.04), mix(colour, (0, 0, 0), 0.25), grain=10).convert("RGBA")
    paste_rounded(img, plate_, x, y, r)


def recess(img, box, r=8):   # a shallow milled pocket for a group of controls, an ice strip along its top
    x, y, w, h = box
    pocket = brushed(w, h, POCKET_TOP, POCKET_BOTTOM, grain=16).convert("RGBA")
    paste_rounded(img, pocket, x, y - Y0, r)
    bevel(img, (x, y - Y0, x + w - 1, y - Y0 + h - 1), r, light=(255, 255, 255, 60), dark=(0, 0, 0, 170), width=2, inset=True)
    ice_bar(img, (x + 14, y + 3, w - 28, 3), 1)


# ---- the backdrops ----
def pad_panel(img):   # the 4x4 pads' plate, bottom left (MAIN, PAD and FX: the same place on each)
    gx0 = GRID_CX - (4 * CELL_W + 3 * GAP) // 2
    anodised_panel(img, (gx0 - 12, GRID_FIRST_CY - CELL_H // 2 - 12, 4 * CELL_W + 3 * GAP + 24, 4 * CELL_H + 3 * GAP + 24), 12)


def main_backdrop():
    img = faceplate()
    glass_panel(img, (HEAD_X, 92, HEAD_W, 44), 8)                      # the sound's name
    glass_panel(img, SCREEN, 10, ice=True)
    d = ImageDraw.Draw(img)
    x, y, w, h = WV
    y -= Y0
    d.rectangle([x, y, x + w - 1, y + h - 1], fill=(5, 6, 8, 255))
    for k in range(1, 8):   # a faint graticule
        gx = x + w * k // 8
        d.line([(gx, y), (gx, y + h - 1)], fill=(17, 18, 22, 255))
    d.line([(x, y + h // 2), (x + w - 1, y + h // 2)], fill=(30, 32, 37, 255))
    d.rectangle([x, MK_Y - Y0, x + w - 1, MK_Y - Y0 + MK_H - 1], fill=(12, 13, 17, 255))
    pad_panel(img)
    glass_panel(img, (536, 400, 474, 34), 6)                          # the pad line (BANK and its buttons beside it)
    recess(img, (536, 436, 716, 104), 10)                              # chop: mode, its setting, zoom
    recess(img, (536, 546, 716, 114), 10)                              # the knobs, the key, the chop buttons
    recess(img, (536, 664, 716, 48), 10)                               # stop, play mode, drums
    return img


def frames_backdrop(boxes, glass_boxes=(), ice_glass=()):
    img = faceplate()
    for b in glass_boxes:
        glass_panel(img, b, 8)
    for b in ice_glass:
        glass_panel(img, b, 10, ice=True)
    for b in boxes:
        recess(img, b, 10)
    return img


# ---- control looks ----
def knob_images():
    """A turned-aluminium knob: knurled dark skirt, concentric-brushed cap, a red pointer (drawn pointing up), at 256 px;
    and its base, a tick scale over 270 degrees."""
    S = 256
    c = S / 2
    k = blank(S, S)
    R = S * 0.43
    sh = blank(S, S)
    ImageDraw.Draw(sh).ellipse([c - R, c - R + 8, c + R, c + R + 8], fill=(0, 0, 0, 150))
    k.alpha_composite(sh.filter(ImageFilter.GaussianBlur(7)))
    d = ImageDraw.Draw(k)
    d.ellipse([c - R, c - R, c + R, c + R], fill=(20, 21, 24, 255))
    for i in range(72):   # the knurling
        a = 2 * math.pi * i / 72
        d.line([(c + (R - 9) * math.cos(a), c + (R - 9) * math.sin(a)), (c + R * math.cos(a), c + R * math.sin(a))], fill=(62, 66, 72, 255), width=2)
    rc = R * 0.8
    cap = blank(S, S)
    cd = ImageDraw.Draw(cap)
    for j in range(int(rc), 0, -1):   # concentric brushing in ice-blue steel, lit from the top left
        t = j / rc
        n = random.randint(-10, 10)
        cd.ellipse([c - j, c - j, c + j, c + j], outline=tuple(min(255, v + n) for v in mix(ICE_LO, ICE_HI, 0.35 + 0.65 * (1 - t))) + (255,))
    light = blank(S, S)
    ImageDraw.Draw(light).polygon([(0, 0), (S, 0), (0, S)], fill=(255, 255, 255, 40))
    cap.alpha_composite(light)
    m = Image.new("L", (S, S), 0)
    ImageDraw.Draw(m).ellipse([c - rc, c - rc, c + rc, c + rc], fill=255)
    k.paste(cap, (0, 0), m)
    d = ImageDraw.Draw(k)
    d.ellipse([c - rc, c - rc, c + rc, c + rc], outline=(232, 246, 255, 255), width=2)
    d.arc([c - rc - 2, c - rc - 2, c + rc + 2, c + rc + 2], 20, 200, fill=(10, 14, 18, 200), width=3)
    d.line([(c, c - rc * 0.92), (c, c - rc * 0.35)], fill=(14, 30, 44, 255), width=10)
    d.ellipse([c - 5, c - rc * 0.92 - 4, c + 5, c - rc * 0.92 + 6], fill=(14, 30, 44, 255))
    save(k, "knob.png")
    b = blank(S, S)
    d = ImageDraw.Draw(b)
    for i in range(11):
        a = math.radians(135 + 27 * i)
        r0, r1 = S * 0.455, S * 0.5 - 2
        d.line([(c + r0 * math.cos(a), c + r0 * math.sin(a)), (c + r1 * math.cos(a), c + r1 * math.sin(a))],
               fill=(176, 222, 246, 255) if i in (0, 5, 10) else (150, 160, 170, 255), width=5 if i in (0, 5, 10) else 3)
    save(b, "knob_base.png")


def plate(w, h, r, top, bottom, grain=20, edge=(0, 0, 0, 150), light=(255, 255, 255, 170)):
    im = blank(w, h)
    paste_rounded(im, brushed(w, h, top, bottom, grain=grain, coarse=30, fine=6).convert("RGBA"), 0, 0, r)
    bevel(im, (0, 0, w - 1, h - 1), r, light=light, dark=edge, width=2)
    return im


def button_looks():
    save(plate(160, 48, 7, STEEL_HI, STEEL_LO, light=(255, 255, 255, 110), edge=(0, 0, 0, 190)), "btn.png")
    save(plate(160, 48, 7, ICE_HI, ICE_LO, grain=16, light=(255, 255, 255, 200)), "btn_on.png")
    save(plate(120, 40, 6, STEEL_HI, STEEL_LO, light=(255, 255, 255, 110), edge=(0, 0, 0, 190)), "seg.png")
    save(plate(120, 40, 6, ICE_HI, ICE_LO, grain=16, light=(255, 255, 255, 200)), "seg_on.png")
    w, h = 72, 34   # a slide switch: a dark slot, a steel thumb left (off) or right over an ice-blue lamp (on)
    for on in (0, 1):
        im = blank(w, h)
        ImageDraw.Draw(im).rounded_rectangle([0, 0, w - 1, h - 1], radius=h // 2, fill=(26, 27, 30, 255))
        bevel(im, (0, 0, w - 1, h - 1), h // 2, light=(255, 255, 255, 120), dark=(0, 0, 0, 200), width=2, inset=True)
        if on:
            glow = blank(w, h)
            ImageDraw.Draw(glow).rounded_rectangle([4, 4, w - 5, h - 5], radius=h // 2 - 4, fill=(120, 200, 240, 220))
            im.alpha_composite(glow)
        im.alpha_composite(plate(h - 4, h - 4, (h - 4) // 2, STEEL_HI, STEEL_LO, grain=14, light=(255, 255, 255, 120)), (w - h + 2 if on else 2, 2))
        save(im, "tog_on.png" if on else "tog.png")


# ---- the screen's filmstrips ----
def wave_strip():
    """76 frames of 4 x 160: 0 empty, then 25 heights x 3 colours, a mirrored bar edge to edge, its core brighter."""
    cw, ch = COL_W, WV[3]
    strip = blank(cw, ch * WAVE_FRAMES)
    d = ImageDraw.Draw(strip)
    mid = ch // 2
    for f in range(1, WAVE_FRAMES):
        lv, col = (f - 1) // 3 + 1, (f - 1) % 3
        half = max(1, round(lv * (mid - 3) / 25))
        c = WAVE_COLS[col]
        for y in range(mid - half, mid + half + 1):
            t = abs(y - mid) / max(1, half)
            k = 1.0 - 0.45 * t ** 1.6
            d.line([(0, f * ch + y), (cw - 1, f * ch + y)], fill=tuple(min(255, int(v * k + 30 * (1 - t) ** 4)) for v in c) + (255,))
    save(strip, "wv_strip.png")


FX_NAMES = ["LOFI", "TELEPHONE", "LOW PASS", "HIGH PASS", "BAND PASS", "VINTAGE", "LOW SHELF", "HIGH SHELF", "NOTCH", "SWEEP",
            "RISER", "ROBOT", "RADIO", "V CURVE", "WARP-SPEED", "TAPE"]
FXP_W, FXP_H, FXP_X0, FXP_Y0 = 196, 36, 165, 195   # the FX page's preset buttons (enum_h cx=560 cy=213 sw=196 sh=36 rows=4)


def fx_preset_pictures():
    """The FX page's 16 presets as pictures under clear buttons, so more than one can be lit: 0 off (a steel button), 1 the
    pad's own (lit ice blue, like a picked option), 2 layered after it (steel, ringed and lettered in ice blue, a +)."""
    f = font(15, TITILLIUM)
    for s, name in enumerate(FX_NAMES):
        for fr in range(3):
            img = plate(FXP_W, FXP_H, 6, ICE_HI, ICE_LO, grain=16, light=(255, 255, 255, 200)) if fr == 1 else                 plate(FXP_W, FXP_H, 6, STEEL_HI, STEEL_LO, light=(255, 255, 255, 110), edge=(0, 0, 0, 190))
            d = ImageDraw.Draw(img)
            if fr == 2:
                d.rounded_rectangle([1, 1, FXP_W - 2, FXP_H - 2], radius=6, outline=ICE_HI + (255,), width=3)
            txt = ("+ " if fr == 2 else "") + name
            tw = d.textlength(txt, font=f)
            colour = (12, 26, 36, 255) if fr == 1 else ICE_HI + (255,) if fr == 2 else (238, 243, 247, 255)
            d.text(((FXP_W - tw) / 2, FXP_H / 2), txt, fill=colour, font=f, anchor="lm")
            save(img, "fp_%02d_%d.png" % (s, fr))
    for st, a in (("on", 2), ("off", 2)):   # the clear buttons over them
        c = blank(FXP_W, FXP_H)
        ImageDraw.Draw(c).rectangle([0, 0, FXP_W - 1, FXP_H - 1], fill=(0, 0, 0, a))
        save(c, "fxcell_%s.png" % st)


def overlay_strip():
    """68 frames of 4 x 160 over a column: 0 none, 1 played, 2..17 playhead (pad colour), 18..33 chop line (pad colour),
    34..49 chop line over played, 50/51 grey (unused), 52..67 another bank's line (its pad's colour, faded, played or
    not: the builder caps a 160 px strip at 76 frames). Lines are the column's left 2 px."""
    cw, ch = COL_W, WV[3]
    strip = blank(cw, ch * OV_FRAMES)
    d = ImageDraw.Draw(strip)

    def wash(f, x0=0):
        for y in range(ch):
            a = int(58 + 34 * (1 - abs(y - ch / 2) / (ch / 2)))
            d.line([(x0, f * ch + y), (cw - 1, f * ch + y)], fill=(255, 255, 255, a))

    def line(f, colour, alpha=255, width=2):
        d.rectangle([0, f * ch, width - 1, f * ch + ch - 1], fill=colour + (alpha,))

    wash(1)
    for s in range(16):
        wash(2 + s, 2)
        line(2 + s, mix(PADS[s], (255, 255, 255), 0.45))
        d.polygon([(0, (2 + s) * ch), (3, (2 + s) * ch), (0, (2 + s) * ch + 5)], fill=(255, 255, 255, 255))
        line(18 + s, PADS[s])
        wash(34 + s, 2)
        line(34 + s, PADS[s])
    line(50, (150, 154, 160), 170, 1)
    wash(51, 1)
    line(51, (150, 154, 160), 200, 1)
    for s in range(16):   # another bank's chop: its pad's colour, faded (52..67)
        line(52 + s, mix(PADS[s], (40, 42, 46), 0.35), 150)
    save(strip, "ov_strip.png")


def tab_strip():
    """257 frames of 56 x 28: 0 none, else 1 + (pad * 8 + column in the slot) * 2 + selected: a flag in the pad's colour
    whose staff (its left edge) is on the chop's column; the selected one lighter, ringed in white. Its number is a meter of
    its own over it (num_strip: 1..64 through the banks)."""
    strip = blank(MK_W, MK_H * MK_FRAMES)
    for f in range(1, MK_FRAMES):
        idx, sel = divmod(f - 1, 2)
        pad, off = divmod(idx, COLS_PER_MARK)
        im = blank(MK_W * 4, MK_H * 4)   # drawn at 4x, then scaled down
        d = ImageDraw.Draw(im)
        x0 = off * COL_W * 4
        c = PADS[pad]
        body = mix(c, (255, 255, 255), 0.25) if sel else c
        d.rectangle([x0, 0, x0 + 7, 4 * 26], fill=body + (255,))   # the staff, from the waveform's edge
        d.rounded_rectangle([x0, 4 * 5, x0 + 4 * 24, 4 * 25], radius=4 * 4, fill=body + (255,))
        d.rectangle([x0, 4 * 5, x0 + 4 * 6, 4 * 25], fill=body + (255,))   # square against the staff
        if sel:
            d.rounded_rectangle([x0 - 2, 4 * 5 - 2, x0 + 4 * 24 + 2, 4 * 25 + 2], radius=4 * 4, outline=(255, 255, 255, 255), width=7)
        im = im.resize((MK_W, MK_H), Image.LANCZOS)
        strip.alpha_composite(im, (0, f * MK_H))
    save(strip, "mk_strip.png")


def num_strip():
    """513 frames of 56 x 16, over the tab row (MN_DY down): 0 none, else 1 + chop * 8 + column in the slot: the chop's number
    (1..64 through the four banks), dark, centred on its flag."""
    strip = blank(MK_W, MN_H * MN_FRAMES)
    d = ImageDraw.Draw(strip)
    big, small = font(15), font(13)
    for f in range(1, MN_FRAMES):
        chop, off = divmod(f - 1, COLS_PER_MARK)
        t = str(chop + 1)
        fn = big if chop < 9 else small
        tw = d.textlength(t, font=fn)
        tx = off * COL_W + 2 + (22 - tw) / 2
        for dx in (0, 0.6):   # a heavier number
            d.text((tx + dx, f * MN_H + (5 - MN_DY) + (0 if chop < 9 else 1)), t, fill=(12, 12, 14, 255), font=fn)
    save(strip, "mn_strip.png")


# ---- the pads (pictures) ----
def cell_rect(o):   # the grid as the skin lays out enum_h rows=4 (options top-down); pads go bottom-up like MPC's
    r, c = divmod(o, 4)
    x0 = GRID_CX - (4 * CELL_W + 3 * GAP) // 2
    return (x0 + c * (CELL_W + GAP), GRID_FIRST_CY - CELL_H // 2 + r * (CELL_H + GAP), CELL_W, CELL_H)


def grid_slot(o):
    return (3 - o // 4) * 4 + o % 4


def pad_images():
    num = font(24)
    for s in range(16):   # pad s of bank b, frame 3 * b + (0 no chop, 1 its chop (its colour), 2 playing (lit)): its own number
        c = PADS[s]
        for fb in range(12):
            bank, f = divmod(fb, 3)
            img = blank(CELL_W, CELL_H)
            d = ImageDraw.Draw(img)
            d.rounded_rectangle([2, 2, CELL_W - 3, CELL_H - 3], radius=9, fill=((30, 31, 35) if f < 2 else mix(c, (255, 255, 255), 0.15)) + (255,))
            if f < 2:
                for y in range(4, CELL_H - 4):   # soft rubber shading
                    d.line([(5, y), (CELL_W - 6, y)], fill=mix((44, 45, 50), (24, 25, 28), (y - 4) / (CELL_H - 8)) + (255,))
            if f == 1:
                tint = blank(CELL_W, CELL_H)
                ImageDraw.Draw(tint).rounded_rectangle([2, 2, CELL_W - 3, CELL_H - 3], radius=9, fill=c + (34,))
                img.alpha_composite(tint)
                ImageDraw.Draw(img).rounded_rectangle([14, CELL_H - 13, CELL_W - 15, CELL_H - 9], radius=2, fill=c + (255,))
            if f == 2:
                glow = blank(CELL_W, CELL_H)
                ImageDraw.Draw(glow).ellipse([10, 4, CELL_W - 10, CELL_H + 30], fill=(255, 255, 255, 90))
                img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(14)))
            bevel(img, (2, 2, CELL_W - 3, CELL_H - 3), 9, light=(255, 255, 255, 60 if f < 2 else 140), dark=(0, 0, 0, 160), width=2)
            tc = (92, 95, 102, 255) if f == 0 else c + (255,) if f == 1 else (16, 16, 18, 255)
            ImageDraw.Draw(img).text((12, 8), str(16 * bank + s + 1), fill=tc, font=num)
            save(img, "lp_%02d_%d.png" % (s, fb))
    ring = blank(CELL_W, CELL_H)
    ImageDraw.Draw(ring).rounded_rectangle([1, 1, CELL_W - 2, CELL_H - 2], radius=9, outline=(255, 255, 255, 255), width=4)
    save(ring, "cell_on.png")
    fx_preset_pictures()
    off = blank(CELL_W, CELL_H)
    ImageDraw.Draw(off).rectangle([0, 0, CELL_W - 1, CELL_H - 1], fill=(0, 0, 0, 2))
    save(off, "cell_off.png")
    tab = blank(MK_STEP - GAP, MK_H)
    ImageDraw.Draw(tab).rectangle([0, 0, MK_STEP - GAP - 1, MK_H - 1], fill=(0, 0, 0, 2))
    save(tab, "tabcell.png")
    img = blank(4, 8)   # the drag strip: two near-invisible 4x4 frames
    ImageDraw.Draw(img).rectangle([0, 0, 3, 7], fill=(0, 0, 0, 2))
    save(img, "clear.png")


def tile_and_title():
    tile = brushed(270, 110, PLATE_TOP, PLATE_BOTTOM).convert("RGBA")
    d = ImageDraw.Draw(tile)
    d.rounded_rectangle([10, 10, 259, 84], radius=6, fill=(8, 9, 12, 255))
    for i in range(12, 258, 3):
        h = 28 * abs(math.sin(i / 13.0)) * (0.35 + 0.65 * abs(math.sin(i / 47.0))) + 2
        d.rectangle([i, 47 - h, i + 2, 47 + h], fill=WAVE_COLS[(i // 30) % 3] + (255,))
    for k, x in enumerate((14, 80, 146, 212)):
        d.rounded_rectangle([x, 89, x + 26, 105], radius=4, fill=PADS[k * 3] + (255,))
        d.text((x + 9, 90), str(k + 1), fill=(16, 16, 18), font=font(13))
    tile.convert("RGB").save(os.path.join(HERE, "tile.png"))


def browse_backdrop():   # the highlighted file's name up top (wider than the sound's on MAIN)
    return frames_backdrop([(1100, 196, 160, 512)], [(HEAD_X, 92, BR_HEAD_W, 44), (36, 150, 1224, 36)], [BROWSE_LIST])


def pads_page_backdrop(boxes):   # PAD and FX: their groups, the pads where MAIN has them
    img = frames_backdrop(boxes, [(450, 92, 380, 48)])
    pad_panel(img)
    return img


knob_images()
button_looks()
wave_strip()
overlay_strip()
tab_strip()
num_strip()
pad_images()
tile_and_title()

SOUND_BOXES = [(20, 152, 620, 200), (650, 152, 610, 200), (20, 360, 620, 170), (650, 360, 610, 170), (20, 538, 1240, 168)]
# PAD and FX: the groups above and right of the pads (which sit where MAIN has them, x 19..523, y 397..707)
PAD_BOXES = [(20, 152, 500, 236), (530, 152, 730, 190), (535, 350, 360, 217), (905, 350, 355, 217), (535, 575, 725, 132)]
FX_BOXES = [(20, 152, 1240, 236), (535, 397, 725, 310)]
main_backdrop().convert("RGB").save(os.path.join(HERE, "bg_main.png"), optimize=True)
pads_page_backdrop(PAD_BOXES).convert("RGB").save(os.path.join(HERE, "bg_pad.png"), optimize=True)
pads_page_backdrop(FX_BOXES).convert("RGB").save(os.path.join(HERE, "bg_fx.png"), optimize=True)
frames_backdrop(SOUND_BOXES, [(SOUND_HEAD_X, 92, SOUND_HEAD_W, 44)]).convert("RGB").save(os.path.join(HERE, "bg_sound.png"), optimize=True)
browse_backdrop().convert("RGB").save(os.path.join(HERE, "bg_browse.png"), optimize=True)

open(os.path.join(HERE, "skin.css"), "w").write("""/* ChronoStretch 0.7 (tools/html_art/default.css lists every class): dark brushed steel, light engraved type, ice-blue accents. */
:root { --sheen: 0; --radius: 6px; --title-size: 16px; --label-size: 18px; --seg-size: 16px; --button-size: 17px;
        --shadow: drop-shadow(0 2px 3px rgba(0, 0, 0, 0.6)); }
.frame-border { fill: none; stroke: none; }
.frame-rule { stroke: #3a3e44; }
.frame-title { font-size: 16px; font-weight: 700; fill: #a6dcf6; letter-spacing: 0.32em; }
.text { fill: #dfe6ec; font-weight: 600; letter-spacing: 0.16em; }
.seg-tx { font-weight: 700; letter-spacing: 0.06em; }
.button-tx { font-weight: 700; letter-spacing: 0.16em; fill: #eef3f7; }
.box { fill: #07080b; stroke: #000000; stroke-width: 1.5; }
.arrow-bg { fill: #2a2d33; stroke: #000000; stroke-width: 1; }
.arrow { fill: #a6dcf6; }
.box-label { fill: #c9d2d9; letter-spacing: 0.2em; }
""")


# ---- layout.conf ----
THEME = """art_css=art/skin.css
theme_bg=3c3f44
theme_panel=3c3f44
theme_line=2a2d32
theme_ink=e9eef2
theme_ink_dim=a6dcf6
theme_ink_faint=7d868f
theme_accent=8fd0f0
theme_accent_hi=a6dcf6
theme_knob_face=8fd0f0
theme_knob_ring=1c1e22
theme_knob_dot=0e1e2c
theme_lcd=07080b
theme_seg_active=a6dcf6
theme_seg_inactive=4c5056
theme_seg_active_tx=0c1a24
theme_btn_text=eef3f7
knob_img=art/knob.png
knob_base=art/knob_base.png
seg_img=art/seg.png
seg_img_on=art/seg_on.png
button_img=art/btn.png
button_img_on=art/btn_on.png
toggle_img=art/tog.png
toggle_img_on=art/tog_on.png"""
L = ["# ChronoStretch 0.7, written by art/make_art.py (edit that, not this).", THEME]
MAIN_Q = ["zoom", "scroll", "p_pos", "p_nudge", "sensitivity", "chop_mode", "pitch", "volume", "half", "drums",   # Q-Links 1-4: the view and the chop
          "play_mode", "sync", "chop_div", "cue_count"]
L.append("qlinks_track = " + ",".join(MAIN_Q))
# options of a control shown on two pages: one width everywhere (the builder draws one set of images per parameter)
SW = {"sync": 58, "play_mode": 104, "drums": 62, "bank": 38}


Image.open(os.path.join(HERE, "logo.png")).resize(LOGO[2:], Image.LANCZOS).save(os.path.join(HERE, "logo_s.png"))   # the logo at its size


def head(bg):
    return ['art file="art/%s" x=0 y=%d w=%d h=%d' % (bg, Y0, W, H), 'art file="art/logo_s.png" x=%d y=%d w=%d h=%d' % LOGO]


def sound_stepper(x, w):
    return 'stepper cx=%d cy=114 w=%d h=44 label="" key=sound talign=center tsize=22 tcolor=ecf0f4' % (x + w // 2, w)


def pad_grid():   # the pads, where MAIN has them: their lamps (each its number) and the grid that selects one
    out = []
    for o in range(16):
        s = grid_slot(o)
        x, y, w, h = cell_rect(o)
        out.append('picture x=%d y=%d w=%d h=%d key=lp%02d files="%s"' % (x, y, w, h, s, ",".join("art/lp_%02d_%d.png" % (s, f) for f in range(12))))
    out.append('enum_h cx=%d cy=%d label="" key=pad_tap sw=%d sh=%d rows=4' % (GRID_CX, GRID_FIRST_CY, CELL_W, CELL_H))
    return out


def pad_head(bg):   # PAD and FX: the selected pad and the bank
    return head(bg) + ['stepper cx=640 cy=116 w=380 h=48 label="" key=sel_pad talign=center tsize=26 tcolor=a8dcf5 tweight=bold',
                       'button cx=905 cy=116 w=%d h=%d label="STOP" key=stop' % (BTN_W, BTN_H),
                       'text cx=990 cy=111 label="BANK"', 'enum_h cx=1110 cy=116 label="" key=bank sw=%d' % SW["bank"]]



TD_W = MK_STEP - 2                       # a tab drag strip: one per tab slot
L.append("[tab MAIN]")
L += head("bg_main.png")
L.append(sound_stepper(HEAD_X, HEAD_W))
L.append('text cx=820 cy=109 label="SYNC"')   # top right, in the top bar: the tempo
L.append('enum_h cx=912 cy=114 label="" key=sync sw=%d' % SW["sync"])
L.append('text cx=1064 cy=109 label="1/2 SPEED"')   # half the project's tempo: double-time patterns
L.append('toggle cx=1186 cy=114 label="" key=half ns=0 w=72 h=34')   # a switch like FX ON's
L.append('readout cx=640 cy=172 w=1160 h=30 label="" key=disp_info box=0 tsize=21 tcolor=dfe4ea talign=center')
for i in range(NCOL):
    L.append('meter cx=%d cy=%d w=%d h=%d key=wv%03d strip=art/wv_strip.png frames=%d'
             % (WV[0] + COL_W * i + COL_W // 2, WV[1] + WV[3] // 2, COL_W, WV[3], i, WAVE_FRAMES))
for i in range(NCOL):
    L.append('meter cx=%d cy=%d w=%d h=%d key=ov%03d strip=art/ov_strip.png frames=%d'
             % (WV[0] + COL_W * i + COL_W // 2, WV[1] + WV[3] // 2, COL_W, WV[3], i, OV_FRAMES))
for k in range(NMARK):
    L.append('meter cx=%d cy=%d w=%d h=%d key=mk%02d strip=art/mk_strip.png frames=%d'
             % (WV[0] + MK_STEP * k + MK_W // 2, MK_Y + MK_H // 2, MK_W, MK_H, k, MK_FRAMES))
for k in range(NMARK):   # each tab's number, over it
    L.append('meter cx=%d cy=%d w=%d h=%d key=mn%02d strip=art/mn_strip.png frames=%d'
             % (WV[0] + MK_STEP * k + MK_W // 2, MK_Y + MN_DY + MN_H // 2, MK_W, MN_H, k, MN_FRAMES))
TD_BOXES = []
for k in range(NMARK):   # over each tab slot, a drag strip: a drag starting there grabs that tab's chop
    x = WV[0] + MK_STEP * k
    TD_BOXES.append([x, MK_Y, TD_W, MK_H])
    L.append('slider_h cx=%d cy=%d w=%d h=8 label="" key=td%02d ns=0 vs=12 img=art/clear.png base=art/clear.png' % (x + TD_W // 2, MK_Y + MK_H // 2, TD_W, k))
L += pad_grid()
L.append('readout cx=773 cy=417 w=452 h=30 label="" key=disp_pad box=0 tsize=17 tcolor=a8dcf5 talign=left')
L.append('text cx=1050 cy=412 label="BANK"')   # text sits ~5 px low beside a button's words: raised to line up
L.append('enum_h cx=1172 cy=417 label="" key=bank sw=%d' % SW["bank"])
RAND_W = 104   # RANDOM CHOPS (MAIN and SOUND: one size)
L.append('text cx=658 cy=452 label="CHOP MODE"')   # its options two by two, RANDOM beside them
L.append('enum_h cx=658 cy=488 label="" key=chop_mode sw=100 sh=28 rows=2')
L.append('button cx=842 cy=500 w=%d h=%d label="RANDOM" key=chop_rand' % (RAND_W, BTN_H))
for mode, key, name in (("TRANSIENT", "sensitivity", "SENSITIVITY"), ("EQUAL", "cue_count", "CUES"), ("BPM", "chop_div", "CHOP EVERY"),
                        ("MANUAL", "cue_count", "MAX CHOPS")):
    L.append('knob cx=966 cy=466 r=16 label="%s" key=%s bw=112 ns=21 vs=19 when=chop_mode:%s' % (name, key, mode))
L.append('text cx=1136 cy=466 label="ZOOM"')
L.append('button cx=1072 cy=506 w=42 h=36 label="-" key=zoom_out')
L.append('readout cx=1136 cy=506 w=80 h=36 label="" key=zoom box=1 tsize=17 tcolor=dfe4ea talign=center')
L.append('button cx=1200 cy=506 w=42 h=36 label="+" key=zoom_in')
KNOB_Y = 580
KEY_CX = 756   # the key, and UNDO ADD DEL under it, between SCROLL and CUE
KNOB_ROW = (("scroll", "SCROLL", 588), ("p_pos", "CUE", 924), ("p_nudge", "NUDGE", 1014), ("pitch", "PITCH", 1104), ("volume", "VOLUME", 1194))
for k, n, cx in KNOB_ROW:   # CUE and NUDGE: the selected pad's chop
    L.append('knob cx=%d cy=%d r=20 label="%s" key=%s bw=84 ns=21 vs=19' % (cx, KNOB_Y, n, k))


def key_group(cx, cy, w, h=34, gap=30):   # the sound's key, and where KEY - and + (a semitone each, tempo kept) took it
    return ['text cx=%d cy=%d label="KEY"' % (cx, cy - gap),
            'button cx=%d cy=%d w=50 h=%d label="-" key=key_down' % (cx - w // 2 - 31, cy, h),
            'readout cx=%d cy=%d w=%d h=%d label="" key=disp_key box=1 tsize=18 tcolor=dfe4ea talign=center' % (cx, cy, w, h),
            'button cx=%d cy=%d w=50 h=%d label="+" key=key_up' % (cx + w // 2 + 31, cy, h)]


for j, (k, n) in enumerate((("undo", "UNDO"), ("chop_add", "ADD"), ("chop_del", "DEL"))):   # under the KEY
    L.append('button cx=%d cy=640 w=%d h=%d label="%s" key=%s' % (KEY_CX + (j - 1) * (BTN_W + 6), BTN_W, BTN_H, n, k))
L += key_group(KEY_CX, 596, 128, 32, 35)   # the sound's key, KEY over it, its - and + (a semitone each, tempo kept)
L.append('button cx=610 cy=688 w=%d h=%d label="STOP" key=stop' % (BTN_W, BTN_H))
L.append('enum_h cx=800 cy=688 label="" key=play_mode sw=%d' % SW["play_mode"])
L.append('text cx=975 cy=683 label="DRUMS"')
L.append('enum_h cx=1120 cy=688 label="" key=drums sw=%d' % SW["drums"])
L.append('qlinks "MAIN" = ' + ",".join(MAIN_Q))

L.append("[tab PAD]")
L += pad_head("bg_pad.png")
L += pad_grid()
L.append('frame x=20 y=152 w=500 h=236 title="CHOP"')
L.append('knob cx=78 cy=222 r=26 label="CUE" key=p_pos bw=100 ns=16 vs=18')
L.append('knob cx=186 cy=222 r=26 label="NUDGE" key=p_nudge bw=100 ns=16 vs=18')
L.append('enum_h cx=370 cy=234 label="LENGTH" key=p_len sw=78 rows=3')
for k, n, cx in (("chop_add", "ADD", 82), ("chop_del", "DEL", 190), ("undo", "UNDO", 298)):
    L.append('button cx=%d cy=362 w=%d h=%d label="%s" key=%s' % (cx, BTN_W, BTN_H, n, k))
L.append('frame x=530 y=152 w=730 h=190 title="SOUND"')
L.append('readout cx=895 cy=322 w=700 h=26 label="" key=disp_pad box=0 tsize=16 tcolor=a8dcf5 talign=center')
for i, (k, n) in enumerate([("p_level", "LEVEL"), ("p_filter", "FILTER"), ("p_attack", "ATTACK"), ("p_release", "RELEASE")]):
    L.append('knob cx=%d cy=222 r=26 label="%s" key=%s bw=150 ns=17 vs=19' % (625 + 175 * i, n, k))
L.append('frame x=535 y=350 w=360 h=217 title="PITCH & SPEED"')
L.append('knob cx=600 cy=440 r=24 label="PAD PITCH" key=p_key bw=100 ns=15 vs=16')
L.append('knob cx=712 cy=440 r=24 label="PAD SPEED" key=p_speed bw=100 ns=15 vs=16')
L.append('toggle cx=830 cy=426 label="REVERSE" key=p_reverse bw=100 w=72 h=34')
L.append('button cx=830 cy=520 w=100 h=34 label="RESET" key=p_reset')
L.append('frame x=905 y=350 w=355 h=217 title="HOW IT PLAYS"')
L.append('enum_h cx=1082 cy=432 label="MODE" key=p_mode sw=100 rows=2')
L.append('enum_h cx=1082 cy=541 label="CHOKE GROUP" key=p_choke sw=56')
L.append('frame x=535 y=575 w=725 h=132 title="DRUMS AND LAYER (THIS PAD)"')
L.append('enum_h cx=760 cy=650 label="DRUMS" key=p_drums sw=80')
L.append('enum_h cx=1080 cy=650 label="LAYER GROUP" key=p_layer sw=56')   # pads of one group play together
L.append('qlinks "PAD" = p_pos,p_nudge,p_level,p_filter,p_attack,p_release,p_key,p_speed,p_len,p_mode,p_choke,p_drums,p_reverse,p_layer')

L.append("[tab FX]")
L += pad_head("bg_fx.png")
L += pad_grid()
L.append('frame x=20 y=152 w=1240 h=236 title="FX (THIS PAD)"')
for s in range(16):   # the presets' pictures (more than one lit: the pad's own and its layers), under the clear buttons
    L.append('picture x=%d y=%d w=%d h=%d key=fp%02d files="%s"' % (FXP_X0 + s % 4 * (FXP_W + 2), FXP_Y0 + s // 4 * (FXP_H + 2), FXP_W, FXP_H, s,
                                                                    ",".join("art/fp_%02d_%d.png" % (s, f) for f in range(3))))
L.append('enum_h cx=560 cy=213 label="" key=p_fx_pick sw=196 sh=36 rows=4')   # 16 presets, no OFF: the switch beside them
L.append('toggle cx=1060 cy=262 label="FX ON" key=p_fx_on bw=110 w=72 h=34')
L.append('frame x=535 y=397 w=725 h=310 title="PAD FX"')
L.append('readout cx=897 cy=422 w=700 h=28 label="" key=disp_fx box=0 tsize=17 tcolor=a8dcf5 talign=center')
for row, cy in ((("p_fx_amt", "AMOUNT"), ("p_fx_rate", "SWEEP / RISE"), ("p_fx_mix", "MIX"), ("p_fx_res", "RESONANCE")), 474), \
               ((("p_fx_drive", "DRIVE"), ("p_fx_tone", "TONE"), ("p_fx_width", "WIDTH")), 590):
    for j, (k, n) in enumerate(row):
        L.append('knob cx=%d cy=%d r=24 label="%s" key=%s bw=116 ns=15 vs=16' % (600 + 120 * j, cy, n, k))
for k, n, cy in (("fx_copy", "COPY TO ALL PADS", 470), ("fx_clear", "CLEAR ALL FX", 545)):
    L.append('button cx=1150 cy=%d w=196 h=48 label="%s" key=%s' % (cy, n, k))
L.append('button cx=1150 cy=622 w=%d h=%d label="UNDO" key=undo' % (BTN_W, BTN_H))
L.append('qlinks "FX" = p_fx_pick,p_fx_on,p_fx_amt,p_fx_rate,p_fx_mix,p_fx_res,p_fx_drive,p_fx_tone,p_fx_width')

L.append("[tab SOUND]")
L += head("bg_sound.png")
L.append(sound_stepper(SOUND_HEAD_X, SOUND_HEAD_W))
L.append('frame x=20 y=152 w=620 h=200 title="TIME"')
L.append('enum_h cx=130 cy=212 label="SYNC" key=sync sw=%d' % SW["sync"])
L.append('enum_h cx=420 cy=212 label="TIME" key=tempo_mult sw=96')
for k, n, cx in (("quality", "QUALITY", 95), ("transient", "TRANSIENT", 235), ("sound_bpm", "SOUND BPM", 375), ("bpm_fine", "BPM FINE", 515)):
    L.append('knob cx=%d cy=266 r=22 label="%s" key=%s bw=120 ns=15 vs=16' % (cx, n, k))
L.append('frame x=650 y=152 w=610 h=200 title="KEY"')
for k, n, cx in (("key", "MOVE TO KEY", 745), ("sound_key", "SOUND KEY", 955), ("pitch", "PITCH", 1165)):
    L.append('knob cx=%d cy=220 r=24 label="%s" key=%s bw=150 ns=15 vs=16' % (cx, n, k))
L += key_group(955, 328, 200)[1:]
L.append('frame x=20 y=360 w=620 h=170 title="DRUMS AND MIDI KEYS"')
L.append('enum_h cx=170 cy=450 label="DRUMS" key=drums sw=%d' % SW["drums"])
L.append('enum_h cx=480 cy=450 label="MIDI KEYS" key=keymap sw=96')   # ALL 64: a keyboard plays every bank's chops
L.append('frame x=650 y=360 w=610 h=170 title="CHOPS"')
L.append('button cx=710 cy=420 w=%d h=%d label="RANDOM" key=chop_rand' % (RAND_W, BTN_H))
L.append('button cx=710 cy=482 w=%d h=%d label="CLEAR" key=clear_cues' % (BTN_W, BTN_H))
L.append('enum_h cx=905 cy=420 label="CUES" key=cue_count sw=52')
L.append('enum_h cx=905 cy=492 label="SNAP" key=snap sw=104')
L.append('knob cx=1090 cy=420 r=22 label="SENSITIVITY" key=sensitivity bw=100 ns=14 vs=16')
L.append('knob cx=1195 cy=420 r=22 label="CHOP EVERY" key=chop_div bw=100 ns=14 vs=16')
L.append('frame x=20 y=538 w=1240 h=168 title="PLAYING"')
L.append('enum_h cx=150 cy=630 label="PLAY MODE" key=play_mode sw=%d' % SW["play_mode"])
L.append('toggle cx=360 cy=622 label="KEYBOARD" key=keyboard bw=140 w=72 h=34')
L.append('toggle cx=520 cy=622 label="SELECT ON HIT" key=follow bw=150 w=72 h=34')
L.append('knob cx=660 cy=612 r=24 label="VOLUME" key=volume bw=110 ns=15 vs=16')
L.append('knob cx=780 cy=612 r=24 label="DRAG SPEED" key=drag_speed bw=116 ns=15 vs=16')
L.append('button cx=940 cy=598 w=150 h=42 label="LEARN PADS" key=learn_pads')
L.append('button cx=940 cy=656 w=150 h=42 label="RESET PADS" key=reset_pads')
L.append('button cx=1150 cy=622 w=130 h=44 label="RESCAN" key=rescan')
L.append('readout cx=930 cy=558 w=640 h=22 label="" key=disp_pads box=0 tsize=15 tcolor=a8dcf5 talign=right')
L.append('qlinks "SOUND" = key,pitch,sound_key,transient,volume,sync,tempo_mult,quality,drums,cue_count,sensitivity,chop_div,drag_speed,sound_bpm,bpm_fine')

L.append("[tab BROWSE]")
L += head("bg_browse.png")
L.append('readout cx=%d cy=114 w=%d h=44 label="" key=br_cursor box=0 tsize=22 tcolor=ecf0f4 talign=center' % (HEAD_X + BR_HEAD_W // 2, BR_HEAD_W))
L.append('button cx=1194 cy=114 w=%d h=%d label="STOP" key=stop' % (BTN_W, BTN_H))
L.append('readout cx=648 cy=168 w=1200 h=34 label="" key=disp_path box=0 tsize=26 tcolor=bfe6f8 tweight=semibold talign=left')
bx, by, bw, bh = BROWSE_LIST
ROW_TH, ROW_GAP = 46, 4
L.append('list x=%d y=%d w=%d h=%d key=br cols=1 rows=%d th=%d gap=%d tsize=27 tcolor=d7e3ec tweight=semibold talign=left' % (bx + 8, by + 8, bw - 16, 10 * ROW_TH + 9 * ROW_GAP, 10, ROW_TH, ROW_GAP))
BD_BOXES = []   # over each row a touch strip (skin_post.py grows them): a drag scrolls, the data wheel picks, a double tap loads
for r in range(10):
    box = [bx + 8, by + 8 + r * (ROW_TH + ROW_GAP), bw - 16, ROW_TH]
    BD_BOXES.append(box)
    L.append('slider_v cx=%d cy=%d w=8 h=8 label="" key=bd%02d bw=64 ns=0 vs=12 img=art/clear.png base=art/clear.png' % (box[0] + box[2] // 2, box[1] + box[3] // 2, r + 1))
for i, (k, n) in enumerate((("br_load", "LOAD"), ("br_home", "DRIVES"), ("br_up", "BACK"), ("br_pgup", "PAGE UP"), ("br_pgdn", "PAGE DOWN"))):
    L.append('button cx=1180 cy=%d w=136 h=64 label="%s" key=%s' % (238 + 80 * i, n, k))
L.append('knob cx=1180 cy=624 r=24 label="VOLUME" key=volume bw=110 ns=15 vs=16')
L.append('qlinks "BROWSE" = volume')
# every knob's name and value at least 20 and 19 px, for reading at arm's length
L = [re.sub(r" ns=(\d+) vs=(\d+)", lambda m: " ns=%d vs=%d" % (max(20, int(m.group(1))), max(19, int(m.group(2)))), ln) if ln.startswith("knob ") else ln
     for ln in L]
open(os.path.join(PORT, "layout.conf"), "w").write("\n".join(L) + "\n")

# what skin_post.py needs: the pad pictures' file patterns and boxes, the meters' exact strips, the tab drag strips, the grid
pics = {}
for o in range(16):
    s = grid_slot(o)
    pics["lp%02d" % s] = ["lp_%02d_%%d.png" % s, list(cell_rect(o))]
json.dump({"pictures": pics,
           "meters": [{"w": COL_W, "h": WV[3], "frames": WAVE_FRAMES, "file": "wv_strip.png"},
                      {"w": COL_W, "h": WV[3], "frames": OV_FRAMES, "file": "ov_strip.png"},
                      {"w": MK_W, "h": MK_H, "frames": MK_FRAMES, "file": "mk_strip.png"},
                      {"w": MK_W, "h": MN_H, "frames": MN_FRAMES, "file": "mn_strip.png"}],
           "drags": {"prefix": "td", "boxes": TD_BOXES},
           "vdrags": {"prefix": "bd", "open": "bt", "enter": "br_enter", "boxes": BD_BOXES},
           "grids": [{"key": "pad_tap", "on": "cell_on.png", "off": "cell_off.png"},
                     {"key": "p_fx_pick", "on": "fxcell_on.png", "off": "fxcell_off.png"}]},
          open(os.path.join(HERE, "geometry.json"), "w"), indent=1)

# a preview of MAIN for review (build/, not shipped): the backdrop, a made-up waveform with chops, tabs and a playhead
prev = Image.open(os.path.join(HERE, "bg_main.png")).convert("RGBA")
ws, ovs, ms = (Image.open(os.path.join(HERE, f)) for f in ("wv_strip.png", "ov_strip.png", "mk_strip.png"))
chops = [int(NCOL * k / 16) + (k * 7) % 5 for k in range(16)]
for c in range(NCOL):
    env = abs(math.sin(c / 9.0)) * (0.3 + 0.7 * math.exp(-((c % 18) / 6.0))) + 0.08 * random.random()
    lv = max(1, min(25, int(env * 25)))
    f = 1 + 3 * (lv - 1) + (0 if c % 18 < 4 else 1 if c % 7 else 2)
    x = WV[0] + c * COL_W
    prev.alpha_composite(ws.crop((0, f * WV[3], COL_W, (f + 1) * WV[3])), (x, WV[1] - Y0))
    of = 1 if chops[2] <= c <= chops[2] + 9 else 0
    if c in chops:
        of = (34 if of else 18) + chops.index(c)
    if c == chops[2] + 10:
        of = 2 + 2
    if of:
        prev.alpha_composite(ovs.crop((0, of * WV[3], COL_W, (of + 1) * WV[3])), (x, WV[1] - Y0))
for s, c in enumerate(chops):
    k, off = divmod(c, COLS_PER_MARK)
    f = 1 + (s * COLS_PER_MARK + off) * 2 + (s == 2)
    prev.alpha_composite(ms.crop((0, f * MK_H, MK_W, (f + 1) * MK_H)), (WV[0] + k * MK_STEP, MK_Y - Y0))
for o in range(16):
    s = grid_slot(o)
    x, y, w, h = cell_rect(o)
    prev.alpha_composite(Image.open(os.path.join(HERE, "lp_%02d_%d.png" % (s, 2 if s == 2 else 1))), (x, y - Y0))
kn = Image.open(os.path.join(HERE, "knob.png")).resize((56, 56), Image.LANCZOS)
kb = Image.open(os.path.join(HERE, "knob_base.png")).resize((56, 56), Image.LANCZOS)
for _, _, cx in KNOB_ROW:
    prev.alpha_composite(kb, (cx - 28, KNOB_Y - 28 - Y0))
    prev.alpha_composite(kn.rotate(-40 + cx % 90, Image.BICUBIC), (cx - 28, KNOB_Y - 28 - Y0))
seg, segon = (Image.open(os.path.join(HERE, f)).resize((100, 36)) for f in ("seg.png", "seg_on.png"))
for i in range(4):
    prev.alpha_composite(segon if i == 0 else seg, (658 - 101 + i % 2 * 102, 488 + i // 2 * 30 - 18 - Y0))
prev.alpha_composite(Image.open(os.path.join(HERE, "logo_s.png")), (LOGO[0], LOGO[1] - Y0))
os.makedirs(os.path.join(PORT, "build"), exist_ok=True)
prev.convert("RGB").save(os.path.join(PORT, "build", "preview_main.png"))
print("make_art: %d layout lines, %d pictures" % (len(L), len(pics)))
for name in ("pad", "fx"):   # and of PAD and FX: the pads where MAIN has them
    pp = Image.open(os.path.join(HERE, "bg_%s.png" % name)).convert("RGBA")
    for o in range(16):
        s = grid_slot(o)
        x, y, w, h = cell_rect(o)
        pp.alpha_composite(Image.open(os.path.join(HERE, "lp_%02d_%d.png" % (s, 1))), (x, y - Y0))
    pp.alpha_composite(Image.open(os.path.join(HERE, "logo_s.png")), (LOGO[0], LOGO[1] - Y0))
    pp.convert("RGB").save(os.path.join(PORT, "build", "preview_%s.png" % name))
