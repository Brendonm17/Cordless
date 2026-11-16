#!/usr/bin/env python3
"""Generate Cordless LiveArea art (icon0/bg/startup/pic0) with Pillow.

Writes PNGs into sce_sys/ from the constants below. Run from the repo root:

    python3 scripts/gen_livearea.py
"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "assets", "fonts")
MARK_PATH = os.path.join(ROOT, "assets", "vita_clyde_mark.png")
OUT_SCE = os.path.join(ROOT, "sce_sys")
OUT_LA = os.path.join(OUT_SCE, "livearea", "contents")

# Discord dark palette + blurple.
BLURPLE   = (88, 101, 242)     # #5865F2
BLURPLE_D = (64, 74, 194)
BLURPLE_L = (124, 135, 246)
DARK      = (24, 25, 28)       # #18191c
DARK2     = (43, 45, 49)       # #2b2d31
CARD      = (49, 51, 56)       # #313338
WHITE     = (255, 255, 255)
MUTED     = (168, 173, 182)

GINTO_PATHS = (
    os.path.join(FONTS, "GintoNord-Black.ttf"),
    os.path.expanduser("~/Library/Fonts/625933f807781e4a4303f1cb_Ginto Nord Black.ttf"),
    "/Library/Fonts/625933f807781e4a4303f1cb_Ginto Nord Black.ttf",
)

def semi(sz): return ImageFont.truetype(os.path.join(FONTS, "Inter-SemiBold.ttf"), sz)
def reg(sz):  return ImageFont.truetype(os.path.join(FONTS, "Inter-Regular.ttf"), sz)

def ginto(sz):
    for p in GINTO_PATHS:
        if os.path.exists(p):
            return ImageFont.truetype(p, sz)
    return semi(sz)

def clyde_mark(target_w, color=WHITE):
    """Load the extracted Vita-Clyde mark scaled to target_w and tinted to color."""
    src = Image.open(MARK_PATH).convert("RGBA")
    mw = max(1, int(round(target_w)))
    mh = max(1, int(round(mw * src.height / src.width)))
    resized = src.resize((mw, mh), Image.LANCZOS)
    tinted = Image.new("RGBA", (mw, mh), color + (0,))
    tinted.putalpha(resized.getchannel("A"))
    return tinted

def vgrad(w, h, top, bot):
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)],
               fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)))
    return img

def dgrad(w, h, tl, br, n=96):
    """Diagonal gradient top-left(tl) -> bottom-right(br). Rendered small + upscaled
    (smooth, and far faster than per-pixel at full size)."""
    g = Image.new("RGB", (n, n))
    px = g.load()
    for y in range(n):
        for x in range(n):
            t = (x + y) / (2 * (n - 1))
            px[x, y] = tuple(int(tl[i] + (br[i] - tl[i]) * t) for i in range(3))
    return g.resize((w, h), Image.BILINEAR)

def glow(size, cx, cy, radius, color, alpha):
    lay = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(lay).ellipse([cx - radius, cy - radius, cx + radius, cy + radius],
                                fill=color + (alpha,))
    return lay.filter(ImageFilter.GaussianBlur(radius * 0.55))

def bubble(draw, box, fill, radius, tail='left'):
    """Rounded speech bubble; tail at bottom-'left', bottom-'right', or None."""
    x0, y0, x1, y1 = box
    draw.rounded_rectangle(box, radius=radius, fill=fill)
    w = x1 - x0
    h = y1 - y0
    if tail == 'left':
        tx = x0 + w * 0.26
        draw.polygon([(tx, y1 - h * 0.10), (tx - w * 0.02, y1 + h * 0.34),
                      (tx + w * 0.24, y1 - h * 0.10)], fill=fill)
    elif tail == 'right':
        tx = x1 - w * 0.26
        draw.polygon([(tx, y1 - h * 0.10), (tx + w * 0.02, y1 + h * 0.34),
                      (tx - w * 0.24, y1 - h * 0.10)], fill=fill)

def dots(draw, cx, cy, r, gap, color):
    for dx in (-1, 0, 1):
        x = cx + dx * gap
        draw.ellipse([x - r, cy - r, x + r, cy + r], fill=color)

def save_indexed(img, path):
    """The Vita's scePromoterUtil rejects RGB/RGBA sce_sys PNGs (install error
    0x8010113d): they must be 8bpp INDEXED (mode 'P'). Quantize to a 256-color
    adaptive palette with Floyd-Steinberg dithering to keep the gradients smooth."""
    img.convert("RGB").quantize(colors=256).save(path, optimize=True)

def logo_tile(s):
    """RGBA rounded-square blurple app-icon tile (Vita-Clyde mark),
    transparent outside the rounded corners so it can be pasted onto any bg."""
    tile = Image.new("RGBA", (s, s), BLURPLE + (255,))
    mark = clyde_mark(s * 0.78, WHITE)
    tile.alpha_composite(mark, ((s - mark.width) // 2, (s - mark.height) // 2))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=int(s * 0.235), fill=255)
    tile.putalpha(mask)
    return tile

# --- icon0.png : 128x128 full-bleed blurple + white Vita-Clyde mark ---------
def make_icon(path, S=128, ss=4):
    W = S * ss
    img = Image.new("RGBA", (W, W), BLURPLE + (255,))
    mark = clyde_mark(W * 0.78, WHITE)
    img.alpha_composite(mark, ((W - mark.width) // 2, (W - mark.height) // 2))
    save_indexed(img.resize((S, S), Image.LANCZOS), path)

# --- startup.png : 280x158 LiveArea cover (mark + "Cordless" wordmark) ----
def make_startup(path, W=280, H=158, ss=4):
    w, h = W * ss, H * ss
    img = dgrad(w, h, (118, 130, 250), (56, 64, 174)).convert("RGBA")
    img.alpha_composite(glow((w, h), w * 0.50, h * 0.50, h * 0.65, (206, 214, 255), 48))

    # Subtle background watermark on the right edge
    wm = clyde_mark(w * 0.52, WHITE)
    wm.putalpha(wm.getchannel("A").point(lambda a: int(a * 0.07)))
    img.alpha_composite(wm, (int(w * 0.68), -int(h * 0.14)))

    # Horizontal lockup matching Discord's logo lockup: [Vita-Clyde]  Cordless
    font = ginto(int(h * 0.18))
    probe = ImageDraw.Draw(img, "RGBA")
    tb = probe.textbbox((0, 0), "Cordless", font=font)
    tw, th = tb[2] - tb[0], tb[3] - tb[1]

    mark = clyde_mark(w * 0.20, WHITE)
    gap = int(w * 0.036)
    total_w = mark.width + gap + tw
    x0 = (w - total_w) // 2
    cy = h // 2
    mx = x0
    my = cy - mark.height // 2
    tx = x0 + mark.width + gap - tb[0]
    ty = cy - (tb[1] + tb[3]) // 2

    # Drop shadow behind mark + text
    sh = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    dy = max(2, int(h * 0.022))
    sh_mark = clyde_mark(mark.width, (18, 20, 54))
    sh_mark.putalpha(sh_mark.getchannel("A").point(lambda a: int(a * 0.65)))
    sh.alpha_composite(sh_mark, (mx, my + dy))
    ImageDraw.Draw(sh, "RGBA").text((tx, ty + dy), "Cordless", font=font, fill=(18, 20, 54, 165))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(h * 0.025)))

    img.alpha_composite(mark, (mx, my))
    ImageDraw.Draw(img, "RGBA").text((tx, ty), "Cordless", font=font, fill=WHITE)
    save_indexed(img.resize((W, H), Image.LANCZOS), path)

# --- bg.png / pic0.png : atmospheric backdrop ---
def make_bg(path, W=960, H=544):
    # Backdrop with the identity lockup in the lower-left.
    img = vgrad(W, H, (32, 34, 40), (15, 16, 19)).convert("RGBA")
    img.alpha_composite(glow((W, H), W * 0.50, H * 0.32, H * 0.72, BLURPLE, 30))  # soft halo

    # Faint oversized Vita-Clyde watermark off the top-right
    wm = clyde_mark(W * 0.44, BLURPLE)
    wm.putalpha(wm.getchannel("A").point(lambda a: int(a * 0.08)))
    img.alpha_composite(wm, (int(W * 0.72), -int(H * 0.10)))

    # Lower-left identity lockup: app-icon tile + Ginto Nord Black wordmark + subtitle.
    s = int(H * 0.15)
    ix, iy = int(W * 0.055), int(H * 0.70)
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle([ix, iy + 6, ix + s, iy + s + 10],
                                         radius=int(s * 0.235), fill=(0, 0, 0, 150))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(14)))
    img.alpha_composite(logo_tile(s), (ix, iy))
    d = ImageDraw.Draw(img, "RGBA")
    tx = ix + s + int(W * 0.022)
    d.text((tx, iy + s * 0.34), "Cordless", font=ginto(int(H * 0.080)), fill=WHITE, anchor="lm")
    d.text((tx, iy + s * 0.78), "Discord for PS Vita", font=reg(int(H * 0.045)),
           fill=MUTED, anchor="lm")
    save_indexed(img, path)

def main():
    os.makedirs(OUT_LA, exist_ok=True)
    make_icon(os.path.join(OUT_SCE, "icon0.png"))
    make_bg(os.path.join(OUT_SCE, "pic0.png"), 960, 544)   # pic0 full screen
    make_bg(os.path.join(OUT_LA, "bg.png"), 840, 500)      # psmobile LiveArea bg size
    make_startup(os.path.join(OUT_LA, "startup.png"))
    print("LiveArea art written to sce_sys/")

if __name__ == "__main__":
    main()
