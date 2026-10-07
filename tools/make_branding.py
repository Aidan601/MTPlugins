#!/usr/bin/env python3
"""Builds the installer's wizard images, the setup icon and the host's About logo from the logo in assets/.
Usage (WSL): python3 tools/make_branding.py"""
import os
from PIL import Image

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
LOGO = os.path.join(REPO, "assets", "logo.png")            # square, icons above the wordmark
ICONS = os.path.join(REPO, "assets", "logo-icons.png")     # square, icons only
WIDE = os.path.join(REPO, "assets", "logo-wide.png")       # icons left of the wordmark
ABOUT_WIDTH = 480                                          # What's new and About show it unscaled (Qt 5.3 rich text scales roughly)
OUT = os.path.join(REPO, "installer", "images")

# Inno Setup's sizes per display scale (100 % to 250 %)
LARGE = [(164, 314), (192, 386), (246, 459), (273, 556), (328, 628), (355, 700), (410, 797)]
SMALL = [(55, 55), (64, 68), (83, 80), (92, 97), (110, 106), (119, 123), (138, 140)]
TOP, BOTTOM = (46, 50, 56), (24, 26, 30)                  # the welcome panel's gradient
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]


def gradient(w, h):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM))
        for x in range(w):
            px[x, y] = c
    return img


def fit(src, size):
    return src.resize((size, size), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    logo = Image.open(LOGO).convert("RGBA")
    icons = Image.open(ICONS).convert("RGBA")
    large, small = [], []
    for w, h in LARGE:
        img = gradient(w, h).convert("RGBA")
        s = round(w * 0.96)
        img.alpha_composite(fit(logo, s), ((w - s) // 2, round(h * 0.30 - s / 2)))
        name = f"wizard-{w}x{h}.bmp"
        img.convert("RGB").save(os.path.join(OUT, name))
        large.append(name)
    for w, h in SMALL:
        img = Image.new("RGBA", (w, h), (255, 255, 255, 255))
        s = min(w, h)
        img.alpha_composite(fit(icons, s), ((w - s) // 2, (h - s) // 2))
        name = f"wizard-small-{w}x{h}.bmp"
        img.convert("RGB").save(os.path.join(OUT, name))
        small.append(name)
    icons.save(os.path.join(OUT, "MTPlugins.ico"), sizes=[(s, s) for s in ICO_SIZES])
    wide = Image.open(WIDE).convert("RGBA")
    wide = wide.crop(wide.getbbox())
    h = round(wide.height * ABOUT_WIDTH / wide.width)
    wide.resize((ABOUT_WIDTH, h), Image.LANCZOS).save(os.path.join(OUT, "MTPlugins-logo.png"))
    print("WizardImageFile=" + ",".join("images\\" + n for n in large))
    print("WizardSmallImageFile=" + ",".join("images\\" + n for n in small))


if __name__ == "__main__":
    main()
