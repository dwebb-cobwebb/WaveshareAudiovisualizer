#!/usr/bin/env python3
"""Generate photorealistic VU meter assets for the Vibe view.

Renders a classic black-face VU dial (bezel, matte face, authentic
non-linear VU scale with red over-zone), a tapered cream needle and a hub
cap at 3x resolution, downsamples with Lanczos for anti-aliasing, and emits
LVGL 8 C image arrays (RGB565, LV_COLOR_16_SWAP=1).

The scale geometry is the real thing: a VU meter's movement responds to
rectified signal current, and 0 VU sits at 71% of full-scale deflection, so
each dB mark lands at fraction 0.71 * 10^(dB/20) of the arc. That is what
gives the genuine compressed-at-the-bottom VU look.

Outputs: src/ui/assets/vu_face.c, vu_needle.c, vu_hub.c (+ vu_assets.h).
"""

import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 3  # supersampling factor

# Final (1x) dimensions
FACE_W, FACE_H = 320, 172
NEEDLE_W, NEEDLE_H = 18, 140
NEEDLE_PIVOT = (9, 128)      # px within needle image (1x)
HUB_D = 34

# Dial geometry (1x, relative to face)
PIVOT = (FACE_W // 2, FACE_H - 16)   # needle pivot on the face
ARC_R = 138                          # radius of the main scale arc
ARC_HALF_DEG = 43                    # arc spans -43..+43 deg from vertical

OUT_DIR = Path(__file__).resolve().parent.parent / "src" / "ui" / "assets"

WIN = "C:/Windows/Fonts"


def font(path, size):
    return ImageFont.truetype(f"{WIN}/{path}", size)


def frac_for_db(db):
    """Arc fraction (0..1) for a dB mark: 0 VU = 71% of full deflection."""
    return 0.71 * (10.0 ** (db / 20.0))


def angle_for_frac(frac):
    """Degrees from vertical (-ARC_HALF..+ARC_HALF) for arc fraction 0..1."""
    return -ARC_HALF_DEG + frac * (2 * ARC_HALF_DEG)


def polar(cx, cy, r, deg_from_vertical):
    a = math.radians(deg_from_vertical - 90)  # 0 deg = straight up
    return (cx + r * math.cos(a), cy + r * math.sin(a))


def render_face():
    W, H = FACE_W * SS, FACE_H * SS
    img = Image.new("RGB", (W, H), (8, 8, 9))
    d = ImageDraw.Draw(img)

    # ---- bezel: dark top-lit gradient frame with rounded corners ----
    bez = Image.new("L", (W, H), 0)
    bd = ImageDraw.Draw(bez)
    bd.rounded_rectangle([0, 0, W - 1, H - 1], radius=2 * SS, fill=255)
    grad = Image.new("RGB", (W, H))
    for y in range(H):
        t = y / H
        v = int(58 - 38 * t)   # 58 -> 20
        grad.paste((v, v, v + 2), (0, y, W, y + 1))
    img.paste(grad, (0, 0), bez)

    # bezel inner lip: darker inset ring then face cutout
    inset = 5 * SS
    bd2 = ImageDraw.Draw(img)
    bd2.rounded_rectangle([inset - SS, inset - SS, W - inset + SS, H - inset + SS],
                          radius=7 * SS, fill=(12, 12, 13))

    # ---- matte black face with subtle vertical sheen + vignette ----
    face = Image.new("RGB", (W, H), (0, 0, 0))
    fd = ImageDraw.Draw(face)
    for y in range(H):
        t = y / H
        v = int(26 - 14 * t)   # 26 at top -> 12 at bottom
        fd.line([(0, y), (W, y)], fill=(v, v, v + 1))
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [inset, inset, W - inset, H - inset], radius=6 * SS, fill=255)
    img.paste(face, (0, 0), mask)

    # gentle glow behind the scale (as if lamp-lit from above)
    glow = Image.new("L", (W, H), 0)
    gd = ImageDraw.Draw(glow)
    gcx, gcy = W // 2, int(H * 0.30)
    gd.ellipse([gcx - W // 2, gcy - H // 2, gcx + W // 2, gcy + H // 2], fill=26)
    glow = glow.filter(ImageFilter.GaussianBlur(30 * SS))
    img = Image.composite(Image.new("RGB", (W, H), (46, 44, 40)), img, glow)
    d = ImageDraw.Draw(img)

    cx, cy = PIVOT[0] * SS, PIVOT[1] * SS
    r = ARC_R * SS

    cream = (232, 226, 208)
    dim_cream = (170, 165, 150)
    red = (208, 52, 40)

    # ---- main arc: cream up to 0 VU, red from 0 to +3 ----
    def arc_deg(a):  # PIL angle for our vertical-relative angle
        return a - 90

    a_start = angle_for_frac(0.0)
    a_zero = angle_for_frac(frac_for_db(0))
    a_end = angle_for_frac(1.0)
    bbox = [cx - r, cy - r, cx + r, cy + r]
    d.arc(bbox, arc_deg(a_start), arc_deg(a_zero), fill=cream, width=2 * SS)
    d.arc(bbox, arc_deg(a_zero), arc_deg(a_end), fill=red, width=4 * SS)

    # ---- ticks + labels ----
    marks = [-20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3]
    labeled = {-20: "20", -10: "10", -7: "7", -5: "5", -3: "3",
               -1: "1", 0: "0", 1: "1", 2: "2", 3: "3"}
    f_num = font("arialbd.ttf", 11 * SS)
    for db in marks:
        fr = frac_for_db(db)
        ang = angle_for_frac(fr)
        col = red if db >= 0 else cream
        major = db in (-20, -10, -7, -5, -3, 0, 3)
        tick_in = r - (9 if major else 6) * SS
        tick_out = r + 3 * SS
        p1 = polar(cx, cy, tick_in, ang)
        p2 = polar(cx, cy, tick_out, ang)
        d.line([p1, p2], fill=col, width=(2 * SS if major else SS))
        if db in labeled:
            lx, ly = polar(cx, cy, r + 12 * SS, ang)
            t = labeled[db]
            tb = d.textbbox((0, 0), t, font=f_num)
            d.text((lx - (tb[2] - tb[0]) / 2, ly - (tb[3] - tb[1]) / 2 - tb[1]),
                   t, font=f_num, fill=col)

    # minus / plus end markers
    f_pm = font("arialbd.ttf", 13 * SS)
    mx, my = polar(cx, cy, r - 22 * SS, angle_for_frac(0.02))
    d.text((mx - 4 * SS, my - 7 * SS), "−", font=f_pm, fill=dim_cream)
    px_, py_ = polar(cx, cy, r - 22 * SS, angle_for_frac(0.985))
    d.text((px_ - 4 * SS, py_ - 7 * SS), "+", font=f_pm, fill=red)

    # ---- "VU" legend (classic serif) ----
    f_vu = font("timesbd.ttf", 26 * SS)
    tb = d.textbbox((0, 0), "VU", font=f_vu)
    d.text((cx - (tb[2] - tb[0]) / 2, cy - 66 * SS), "VU", font=f_vu, fill=cream)

    # faint noise so the face doesn't band
    import random
    rnd = random.Random(1)
    px = img.load()
    for _ in range(W * H // 14):
        x, y = rnd.randrange(W), rnd.randrange(H)
        pr, pg, pb = px[x, y]
        n = rnd.randint(-3, 3)
        px[x, y] = (max(0, min(255, pr + n)),
                    max(0, min(255, pg + n)),
                    max(0, min(255, pb + n)))

    return img.resize((FACE_W, FACE_H), Image.LANCZOS)


def render_needle():
    W, H = NEEDLE_W * SS, NEEDLE_H * SS
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = NEEDLE_PIVOT[0] * SS
    tip_y = 4 * SS
    base_y = NEEDLE_PIVOT[1] * SS
    # shadow (offset, blurred later)
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sh)
    sd.polygon([(cx - 2.4 * SS + SS, base_y), (cx + 2.4 * SS + SS, base_y),
                (cx + 0.9 * SS + SS, tip_y), (cx - 0.9 * SS + SS, tip_y)],
               fill=(0, 0, 0, 110))
    sh = sh.filter(ImageFilter.GaussianBlur(1.2 * SS))
    img.alpha_composite(sh)
    # tapered cream blade with darker edge
    d.polygon([(cx - 2.4 * SS, base_y), (cx + 2.4 * SS, base_y),
               (cx + 0.9 * SS, tip_y), (cx - 0.9 * SS, tip_y)],
              fill=(212, 200, 175, 255))
    d.polygon([(cx - 1.2 * SS, base_y), (cx + 1.2 * SS, base_y),
               (cx + 0.45 * SS, tip_y), (cx - 0.45 * SS, tip_y)],
              fill=(245, 238, 220, 255))
    return img.resize((NEEDLE_W, NEEDLE_H), Image.LANCZOS)


def render_hub():
    Dm = HUB_D * SS
    img = Image.new("RGBA", (Dm, Dm), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse([0, 0, Dm - 1, Dm - 1], fill=(16, 16, 18, 255))
    d.ellipse([2 * SS, 2 * SS, Dm - 2 * SS, Dm - 2 * SS], fill=(30, 30, 33, 255))
    # dome highlight upper-left
    hl = Image.new("RGBA", (Dm, Dm), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hl)
    hd.ellipse([Dm * 0.18, Dm * 0.10, Dm * 0.62, Dm * 0.42], fill=(95, 95, 100, 160))
    hl = hl.filter(ImageFilter.GaussianBlur(2.5 * SS))
    img.alpha_composite(hl)
    return img.resize((HUB_D, HUB_D), Image.LANCZOS)


def rgb565_swapped(r, g, b):
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return bytes(((v >> 8) & 0xFF, v & 0xFF))   # LV_COLOR_16_SWAP=1: high, low


def emit_c(img, name, alpha):
    w, h = img.size
    data = bytearray()
    rgba = img.convert("RGBA")
    for y in range(h):
        for x in range(w):
            r, g, b, a = rgba.getpixel((x, y))
            data += rgb565_swapped(r, g, b)
            if alpha:
                data.append(a)
    cf = "LV_IMG_CF_TRUE_COLOR_ALPHA" if alpha else "LV_IMG_CF_TRUE_COLOR"
    lines = [
        "// Auto-generated by scripts/gen_vu_assets.py - do not edit.",
        "// RGB565 with LV_COLOR_16_SWAP=1" + (" + alpha byte" if alpha else "") + ".",
        '#include "lvgl.h"', "",
        f"static const uint8_t {name}_map[] = {{",
    ]
    for i in range(0, len(data), 24):
        lines.append("    " + "".join(f"0x{b:02x}," for b in data[i:i + 24]))
    lines += [
        "};", "",
        f"const lv_img_dsc_t {name} = {{",
        f"    .header.cf = {cf},",
        "    .header.always_zero = 0,",
        f"    .header.w = {w},",
        f"    .header.h = {h},",
        f"    .data_size = sizeof({name}_map),",
        f"    .data = {name}_map,",
        "};", "",
    ]
    (OUT_DIR / f"{name}.c").write_text("\n".join(lines))
    print(f"{name}: {w}x{h} {'RGBA' if alpha else 'RGB'} -> {len(data)} bytes")


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    emit_c(render_face(), "vu_face", alpha=False)
    emit_c(render_needle(), "vu_needle", alpha=True)
    emit_c(render_hub(), "vu_hub", alpha=True)
    (OUT_DIR / "vu_assets.h").write_text("\n".join([
        "// Auto-generated by scripts/gen_vu_assets.py - do not edit.",
        "#ifndef AV_VU_ASSETS_H",
        "#define AV_VU_ASSETS_H",
        '#include "lvgl.h"',
        "extern const lv_img_dsc_t vu_face;",
        "extern const lv_img_dsc_t vu_needle;",
        "extern const lv_img_dsc_t vu_hub;",
        f"#define VU_FACE_W {FACE_W}",
        f"#define VU_FACE_H {FACE_H}",
        f"#define VU_PIVOT_X {PIVOT[0]}",
        f"#define VU_PIVOT_Y {PIVOT[1]}",
        f"#define VU_NEEDLE_PIVOT_X {NEEDLE_PIVOT[0]}",
        f"#define VU_NEEDLE_PIVOT_Y {NEEDLE_PIVOT[1]}",
        f"#define VU_HUB_D {HUB_D}",
        f"#define VU_ARC_HALF_DEG {ARC_HALF_DEG}",
        "#endif",
        "",
    ]))
    print("header written")


if __name__ == "__main__":
    main()
