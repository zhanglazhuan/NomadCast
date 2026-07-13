#!/usr/bin/env python3
"""
Download Material Symbols icons from Google Fonts and convert to LVGL .c files.

Scrapes the Google Fonts CSS API to get the icon font, then renders the glyph
directly — producing the EXACT same output as manual download from fonts.google.com/icons.

Usage:
    python tools/download_icons.py timer wifi play_circle search
    python tools/download_icons.py --all

Dependencies: requests, Pillow, fontTools, brotli
"""

import os
import sys
import re
import argparse
from pathlib import Path
from io import BytesIO

import requests
from PIL import Image, ImageFont, ImageDraw
from fontTools.ttLib import TTFont


# ── Google Fonts CSS API ─────────────────────────────────────────────────────

GOOGLE_FONTS_CSS = (
    "https://fonts.googleapis.com/css2"
    "?family=Material+Symbols+Outlined"
    ":opsz,wght,FILL,GRAD@{size},400,0,0"
    "&icon_names={icon_name}"
)

# Icon name aliases — user-friendly name → Material Symbols canonical name
ICON_ALIASES = {
    "wifi": "wifi",
    "timer": "timer",
    "network_wifi": "wifi",
    "signal_wifi_3_bar": "network_wifi_3_bar",
    "wifi_3_bar": "network_wifi_3_bar",
    "network_wifi_3_bar": "network_wifi_3_bar",
    "home_storage": "home_storage",
    "storage": "home_storage",
    "play_circle": "play_circle",
    "account_circle": "account_circle",
    "download": "download",
    "downloading": "downloading",
    "play_arrow": "play_arrow",
    "settings": "settings",
    "logout": "logout",
    "login": "login",
    "search": "search",
    "battery": "battery_horiz_075",
    "battery_horiz_075": "battery_horiz_075",
    "battery_0_bar": "battery_android_0",
    "battery_1_bar": "battery_android_1",
    "battery_2_bar": "battery_android_2",
    "battery_3_bar": "battery_android_3",
    "battery_4_bar": "battery_android_4",
    "battery_5_bar": "battery_android_5",
    "battery_6_bar": "battery_android_6",
    "battery_full": "battery_android_full",
    "battery_charging_full": "battery_android_bolt",
}


# ── Font scraping ────────────────────────────────────────────────────────────

def _fetch_font_and_codepoint(icon_name, size=24):
    """Fetch the CSS font URL for an icon, download the font subset, and return
    (font_bytes, codepoint) where codepoint is the PUA unicode for the icon glyph.
    """
    css_url = GOOGLE_FONTS_CSS.format(icon_name=icon_name, size=size)
    resp = requests.get(css_url, headers={"User-Agent": "Mozilla/5.0"}, timeout=15)
    resp.raise_for_status()

    # Parse font download URL from CSS @font-face rule
    match = re.search(r"url\((https://[^)]+)\)", resp.text)
    if not match:
        raise RuntimeError(f"Font URL not found in CSS for '{icon_name}'")
    font_url = match.group(1).strip()

    # Download font subset
    font_resp = requests.get(font_url, headers={"User-Agent": "Mozilla/5.0"}, timeout=30)
    font_resp.raise_for_status()
    font_bytes = font_resp.content

    # Find the icon's codepoint (the only PUA glyph in the subset)
    ft = TTFont(BytesIO(font_bytes))
    cmap = ft.getBestCmap()
    codepoint = None
    for cp in cmap:
        if cp >= 0xE000:
            codepoint = cp
            break
    ft.close()

    if codepoint is None:
        raise RuntimeError(f"No PUA glyph found in font for '{icon_name}'")

    return font_bytes, codepoint


def _render_icon_to_png(font_bytes, codepoint, size, output_path):
    """Render a single icon glyph to PNG at the given size, perfectly centered."""
    char = chr(codepoint)

    # Render at a larger size on oversized canvas, then downsample for quality
    render_size = size * 8
    font = ImageFont.truetype(BytesIO(font_bytes), render_size)
    canvas = Image.new("RGBA", (render_size, render_size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(canvas)
    draw.text((0, 0), char, font=font, fill=(0, 0, 0, 255))

    # Find exact pixel bounds
    pixels = canvas.load()
    xmin, ymin, xmax, ymax = render_size, render_size, 0, 0
    for y in range(render_size):
        for x in range(render_size):
            if pixels[x, y][3] > 0:
                if x < xmin: xmin = x
                if y < ymin: ymin = y
                if x > xmax: xmax = x
                if y > ymax: ymax = y

    if xmax < xmin:  # No pixels found (empty glyph)
        canvas.save(output_path, "PNG")
        return output_path

    # Crop to content
    cropped = canvas.crop((xmin, ymin, xmax + 1, ymax + 1))

    # Scale to fit target size with 1px padding
    cw, ch = cropped.size
    pad = 1
    target = size - pad * 2
    scale = min(target / cw, target / ch)
    new_w = max(1, int(cw * scale))
    new_h = max(1, int(ch * scale))
    scaled = cropped.resize((new_w, new_h), Image.LANCZOS)

    # Center on final canvas
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ox = (size - new_w) // 2
    oy = (size - new_h) // 2
    img.paste(scaled, (ox, oy), scaled)

    img.save(output_path, "PNG")
    return output_path


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Download Material Symbols icons via Google Fonts CSS scraping"
    )
    parser.add_argument("icons", nargs="*",
                        help="Icon names (e.g. timer wifi search)")
    parser.add_argument("--all", action="store_true",
                        help="Download all known icons")
    parser.add_argument("--size", type=int, default=24,
                        help="Output icon size in pixels (default: 24)")
    parser.add_argument("--cf", default="ARGB8888",
                        help="LVGL color format (default: ARGB8888)")
    parser.add_argument("--assets-dir", default=None,
                        help="PNG output dir (default: ../assets)")
    parser.add_argument("--c-output-dir", default=None,
                        help=".c output dir (default: sys/uilv/imgs/24x24)")
    parser.add_argument("--tools-dir", default=None,
                        help="Path to LVGLImage.py (default: auto-detect)")
    args = parser.parse_args()

    # Resolve paths
    script_dir = Path(__file__).resolve().parent           # tools/
    repo_dir = script_dir.parent                            # NomadCast/

    assets_dir = Path(args.assets_dir) if args.assets_dir else repo_dir / "assets"
    assets_dir.mkdir(parents=True, exist_ok=True)

    c_output_dir = (Path(args.c_output_dir) if args.c_output_dir
                    else repo_dir / "sys" / "uilv" / "imgs"
                         / f"{args.size}x{args.size}")
    c_output_dir.mkdir(parents=True, exist_ok=True)

    # Find LVGLImage.py
    if args.tools_dir:
        lvgl_tool = Path(args.tools_dir) / "LVGLImage.py"
    else:
        candidates = [
            repo_dir / "tools" / "LVGLImage.py",
            repo_dir / "lvgl" / "scripts" / "LVGLImage.py",
        ]
        lvgl_tool = next((c for c in candidates if c.exists()), None)
        if not lvgl_tool:
            print("[ERROR] Cannot find LVGLImage.py. Use --tools-dir.")
            sys.exit(1)

    print(f"Assets dir : {assets_dir}")
    print(f"C output dir: {c_output_dir}")
    print(f"LVGLImage  : {lvgl_tool}")
    print(f"Icon size  : {args.size}x{args.size}")
    print(f"Color fmt  : {args.cf}")
    print()

    # Resolve icon list
    if args.all:
        icon_names = sorted(set(ICON_ALIASES.values()))
    else:
        icon_names = [ICON_ALIASES.get(n.lower(), n.lower()) for n in args.icons]

    if not icon_names:
        print("No icons. Use --all or list icon names.")
        print(f"Known: {', '.join(sorted(ICON_ALIASES.keys()))}")
        sys.exit(1)

    # Process each icon — scrape font + render glyph
    png_files = []

    for icon in icon_names:
        print(f"[{icon}] Scraping...")
        try:
            font_bytes, codepoint = _fetch_font_and_codepoint(icon, args.size)
        except Exception as e:
            print(f"  [FAIL] {e}")
            continue

        png_path = assets_dir / f"{icon}.png"
        _render_icon_to_png(font_bytes, codepoint, args.size, str(png_path))
        print(f"  -> {png_path} ({png_path.stat().st_size} bytes)")
        png_files.append(str(png_path))

    if not png_files:
        print("\nNo icons downloaded.")
        sys.exit(1)

    # Convert PNGs to LVGL .c
    print(f"\n--- Converting {len(png_files)} PNG(s) to LVGL .c ---\n")

    import subprocess
    for png in png_files:
        icon_name = Path(png).stem
        cmd = [
            sys.executable, str(lvgl_tool),
            png,
            "--name", f"ic_{icon_name}",
            "--cf", args.cf,
            "-o", str(c_output_dir),
        ]
        print(f"  {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode == 0:
            c_file = c_output_dir / f"ic_{icon_name}.c"
            print(f"  -> {c_file} ({c_file.stat().st_size} bytes)")
        else:
            print(f"  [ERROR] {result.stderr.strip()}")

    print(f"\nDone! {len(png_files)} icons processed.")


if __name__ == "__main__":
    main()
