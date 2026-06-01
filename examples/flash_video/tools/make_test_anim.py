#!/usr/bin/env python3
"""make_test_anim.py — Generate a built-in test animation as ext_flash_video.bin.

No video file needed.  Produces a 12-frame rotating rainbow gradient that
loops smoothly, designed to showcase the display's full colour depth and
128x160 resolution at ~9 fps.

The animation is a diagonal HSV gradient (hue varies with x+y position)
that rotates one full revolution over 12 frames, giving a 1.3-second loop.

Usage:
    python tools/make_test_anim.py
    python tools/make_test_anim.py --frames 24 --fps 9 --style checkerboard

Output:
    build/ext_flash_video.bin   (ready for gen_flash.py)

Requires: pip install pillow
"""

import argparse
import colorsys
import math
import os
import struct
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("ERROR: pip install pillow")
    sys.exit(1)

LCD_W, LCD_H  = 128, 160
FLASH_TOTAL   = 0x100000
VIDEO_MAGIC   = 0x5650
VIDEO_HDR_SZ  = 16
FRAME_SZ      = LCD_W * LCD_H * 2
MAX_FRAMES    = (FLASH_TOTAL - VIDEO_HDR_SZ) // FRAME_SZ  # 25


# ── Pixel encoder ─────────────────────────────────────────────────────────────

def to_bgr565(img: Image.Image) -> bytes:
    """Convert PIL RGB Image (128x160) to BGR565 little-endian bytes."""
    img = img.convert("RGB")
    out = bytearray(FRAME_SZ)
    px  = img.load()
    idx = 0
    for y in range(LCD_H):
        for x in range(LCD_W):
            r, g, b = px[x, y]
            word = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3)
            out[idx]     = word & 0xFF
            out[idx + 1] = (word >> 8) & 0xFF
            idx += 2
    return bytes(out)


# ── Animation styles ──────────────────────────────────────────────────────────

def make_rainbow_spin(frame_idx, n_frames):
    """Diagonal HSV gradient rotating one full revolution over n_frames."""
    img  = Image.new("RGB", (LCD_W, LCD_H))
    pix  = img.load()
    phase = frame_idx / n_frames   # 0.0 → 1.0
    for y in range(LCD_H):
        for x in range(LCD_W):
            # Hue = diagonal position + rotation
            hue = (x / (LCD_W - 1) * 0.6 + y / (LCD_H - 1) * 0.4 + phase) % 1.0
            r, g, b = colorsys.hsv_to_rgb(hue, 1.0, 1.0)
            pix[x, y] = (int(r * 255), int(g * 255), int(b * 255))
    return img


def make_checkerboard(frame_idx, n_frames):
    """Colour-cycling 8x8 checkerboard that shifts each frame."""
    img  = Image.new("RGB", (LCD_W, LCD_H))
    pix  = img.load()
    shift = frame_idx * 8 // n_frames   # scroll offset in pixels
    hue_base = frame_idx / n_frames
    for y in range(LCD_H):
        for x in range(LCD_W):
            bx = (x + shift) // 8
            by = (y + shift) // 8
            checker = (bx + by) & 1
            hue = (hue_base + (bx + by) * 0.05) % 1.0
            sat = 1.0 if checker else 0.3
            val = 1.0 if checker else 0.6
            r, g, b = colorsys.hsv_to_rgb(hue, sat, val)
            pix[x, y] = (int(r * 255), int(g * 255), int(b * 255))
    return img


def make_plasma(frame_idx, n_frames):
    """Classic plasma / sine-wave colour effect."""
    img   = Image.new("RGB", (LCD_W, LCD_H))
    pix   = img.load()
    phase = 2 * math.pi * frame_idx / n_frames
    for y in range(LCD_H):
        for x in range(LCD_W):
            v  = math.sin(x / 12.0 + phase)
            v += math.sin(y / 9.0  + phase * 1.3)
            v += math.sin((x + y) / 15.0 + phase * 0.7)
            v += math.sin(math.sqrt(x * x + y * y) / 10.0 + phase * 1.1)
            hue = (v / 8.0 + 0.5) % 1.0
            r, g, b = colorsys.hsv_to_rgb(hue, 1.0, 1.0)
            pix[x, y] = (int(r * 255), int(g * 255), int(b * 255))
    return img


def make_bounce(frame_idx, n_frames):
    """Colourful bouncing ball on a dark gradient background."""
    img  = Image.new("RGB", (LCD_W, LCD_H))
    pix  = img.load()

    # Background: subtle dark gradient
    for y in range(LCD_H):
        for x in range(LCD_W):
            hue = (x / LCD_W * 0.5 + y / LCD_H * 0.5) % 1.0
            r, g, b = colorsys.hsv_to_rgb(hue, 0.8, 0.15)
            pix[x, y] = (int(r * 255), int(g * 255), int(b * 255))

    # Ball: bounces on Lissajous path
    t   = 2 * math.pi * frame_idx / n_frames
    cx  = int((LCD_W - 20) / 2 * (1 + math.sin(t     )) + 10)
    cy  = int((LCD_H - 20) / 2 * (1 + math.sin(t * 1.3)) + 10)
    rad = 14
    hue = (frame_idx / n_frames) % 1.0

    draw = ImageDraw.Draw(img)
    # Glow rings
    for ring in range(4, 0, -1):
        v   = 0.4 + ring * 0.15
        rc, gc, bc = colorsys.hsv_to_rgb(hue, 1.0, v)
        col = (int(rc * 255), int(gc * 255), int(bc * 255))
        r2  = rad + ring * 3
        draw.ellipse([cx - r2, cy - r2, cx + r2, cy + r2], fill=col)
    # Core
    rc, gc, bc = colorsys.hsv_to_rgb(hue, 0.3, 1.0)
    col = (int(rc * 255), int(gc * 255), int(bc * 255))
    draw.ellipse([cx - rad, cy - rad, cx + rad, cy + rad], fill=col)

    return img


STYLES = {
    "rainbow": make_rainbow_spin,
    "checkerboard": make_checkerboard,
    "plasma": make_plasma,
    "bounce": make_bounce,
}


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--style", choices=list(STYLES), default="plasma",
                    help="Animation style (default: plasma)")
    ap.add_argument("--frames", type=int, default=12,
                    help=f"Number of frames (default 12; max {MAX_FRAMES})")
    ap.add_argument("--fps", type=int, default=9,
                    help="Target fps stored in header (default 9)")
    ap.add_argument("--output", default=None,
                    help="Output path (default: build/ext_flash_video.bin)")
    args = ap.parse_args()

    n_frames = min(args.frames, MAX_FRAMES)
    fps      = args.fps

    script_dir  = os.path.dirname(os.path.abspath(__file__))
    example_dir = os.path.dirname(script_dir)
    out_path    = args.output or os.path.join(example_dir, "build", "ext_flash_video.bin")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    make_frame = STYLES[args.style]

    print(f"Generating {n_frames}-frame '{args.style}' animation at {fps} fps "
          f"({n_frames / fps:.1f}s loop)...")

    encoded = []
    for i in range(n_frames):
        img = make_frame(i, n_frames)
        encoded.append(to_bgr565(img))
        print(f"\r  Frame {i + 1}/{n_frames}", end="", flush=True)
    print()

    # Build blob: header + frame data
    header = struct.pack("<8H", VIDEO_MAGIC, n_frames, fps, LCD_W, LCD_H, 0, 0, 0)
    blob   = header + b"".join(encoded)

    total_bytes = len(blob)
    n_pages     = (total_bytes + 255) // 256
    n_sectors   = (total_bytes + 4095) // 4096

    with open(out_path, "wb") as f:
        f.write(blob)

    print(f"Written : {out_path}")
    print(f"  Size  : {total_bytes:,} B  ({n_frames} frames × {FRAME_SZ:,} B + {VIDEO_HDR_SZ} B header)")
    print(f"  Pages : {n_pages}  |  Sectors: {n_sectors}")
    print()
    print("Next steps:")
    print(f"  cd {example_dir}")
    print("  python gen_flash.py")
    print("  flash_vape.bat")


if __name__ == "__main__":
    main()
