#!/usr/bin/env python3
"""prep_frames.py — Convert image or video to ext_flash_video.bin for GT25Q80A.

Takes any image or video file, resizes frames to 128×160, encodes as BGR565
(the GC9107 display format), and writes a 16-byte header + frame data to
build/ext_flash_video.bin.  This blob is written to the external GT25Q80A flash
chip by gen_flash.py / flash_vape.bat — it does NOT modify the firmware binary.

Flash layout (external GT25Q80A, 1 MB):
    0x000000  video header (16 bytes)
    0x000010  frame data   (n_frames × 128×160×2 bytes BGR565 LE)

Maximum capacity:
    (1 048 576 − 16) / 40 960 = 25 full-colour frames
    At 6 fps → ~4 seconds; at 4 fps → ~6 seconds.

Usage:
    python tools/prep_frames.py photo.jpg
    python tools/prep_frames.py clip.mp4 --fps 6 --start 5.0 --duration 4.0

Requires: pip install pillow
For video: ffmpeg must be in PATH.
"""

import argparse
import os
import struct
import subprocess
import sys

try:
    from PIL import Image
except ImportError:
    print("ERROR: pip install pillow")
    sys.exit(1)

LCD_W, LCD_H    = 128, 160
FLASH_TOTAL     = 0x100000          # 1 MB external flash
VIDEO_MAGIC     = 0x5650
VIDEO_HDR_SZ    = 16
FRAME_SZ        = LCD_W * LCD_H * 2
MAX_FRAMES      = (FLASH_TOTAL - VIDEO_HDR_SZ) // FRAME_SZ   # 25


def image_to_bgr565(img: Image.Image) -> bytes:
    """Resize and convert PIL Image → BGR565 little-endian bytes for GC9107."""
    img = img.convert("RGB").resize((LCD_W, LCD_H), Image.LANCZOS)
    out = bytearray(FRAME_SZ)
    pixels = img.load()
    idx = 0
    for y in range(LCD_H):
        for x in range(LCD_W):
            r, g, b = pixels[x, y]
            # GC9107 MADCTL=0x98 (BGR=1): B[15:11] G[10:5] R[4:0]
            px = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3)
            out[idx]     = px & 0xFF
            out[idx + 1] = (px >> 8) & 0xFF
            idx += 2
    return bytes(out)


def extract_frames_from_video(path: str, fps: float, start: float,
                               duration: float, max_frames: int) -> list:
    vf = f"fps={fps},scale={LCD_W}:{LCD_H}:flags=lanczos"
    cmd = [
        "ffmpeg", "-ss", str(start), "-i", path,
        "-t", str(duration),
        "-vf", vf,
        "-frames:v", str(max_frames),
        "-f", "rawvideo", "-pix_fmt", "rgb24",
        "-"
    ]
    try:
        result = subprocess.run(cmd, capture_output=True, timeout=120)
    except FileNotFoundError:
        print("ERROR: ffmpeg not found in PATH.")
        sys.exit(1)

    raw = result.stdout
    frame_bytes = LCD_W * LCD_H * 3
    n = len(raw) // frame_bytes
    frames = []
    for i in range(n):
        chunk = raw[i * frame_bytes: (i + 1) * frame_bytes]
        frames.append(Image.frombytes("RGB", (LCD_W, LCD_H), chunk))
    if not frames:
        print("ERROR: ffmpeg produced no frames.")
        print("ffmpeg stderr:", result.stderr.decode(errors="replace"))
        sys.exit(1)
    return frames


def build_video_blob(frames_bgr565: list, fps: int) -> bytes:
    n = len(frames_bgr565)
    # Header: magic, n_frames, fps, width, height, reserved×3  (8 × u16 = 16 B)
    header = struct.pack("<8H", VIDEO_MAGIC, n, fps, LCD_W, LCD_H, 0, 0, 0)
    assert len(header) == VIDEO_HDR_SZ
    return header + b"".join(frames_bgr565)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="Image (jpg/png/…) or video (mp4/mov/…)")
    ap.add_argument("--fps", type=int, default=6,
                    help="Target playback fps (default 6; hardware limit ~6 fps)")
    ap.add_argument("--start", type=float, default=0.0,
                    help="Video start time in seconds (default 0)")
    ap.add_argument("--duration", type=float, default=10.0,
                    help="Video segment length in seconds (default 10; capped by flash)")
    ap.add_argument("--frame-count", type=int, default=None,
                    help=f"Override max frames (default: as many as fit, max {MAX_FRAMES})")
    ap.add_argument("--output", default=None,
                    help="Output path (default: build/ext_flash_video.bin)")
    args = ap.parse_args()

    script_dir  = os.path.dirname(os.path.abspath(__file__))
    example_dir = os.path.dirname(script_dir)
    out_path    = args.output or os.path.join(example_dir, "build", "ext_flash_video.bin")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    max_frames = args.frame_count if args.frame_count else MAX_FRAMES

    ext = os.path.splitext(args.input)[1].lower()
    image_exts = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp", ".tiff", ".tif"}

    if ext in image_exts:
        print(f"Loading image: {args.input}")
        raw_frames = [Image.open(args.input)]
    else:
        max_video_frames = min(max_frames, int(args.duration * args.fps))
        print(f"Extracting up to {max_video_frames} frame(s) from video at {args.fps} fps "
              f"starting at {args.start}s …")
        raw_frames = extract_frames_from_video(
            args.input, args.fps, args.start, args.duration, max_video_frames)

    if len(raw_frames) > max_frames:
        print(f"Capping to {max_frames} frame(s) (external flash limit).")
        raw_frames = raw_frames[:max_frames]

    print(f"Encoding {len(raw_frames)} frame(s) at {LCD_W}×{LCD_H} BGR565 …")
    encoded = [image_to_bgr565(f) for f in raw_frames]

    blob = build_video_blob(encoded, args.fps)
    if len(blob) > FLASH_TOTAL:
        print(f"ERROR: video blob ({len(blob)} B) exceeds external flash ({FLASH_TOTAL} B).")
        sys.exit(1)

    n_pages   = (len(blob) + 255) // 256
    n_sectors = (len(blob) + 4095) // 4096
    print(f"  Video blob  : {len(blob):,} B  ({len(encoded)} frame(s))")
    print(f"  Pages       : {n_pages}  ({n_sectors} sectors to erase)")
    print(f"  Frames fit  : {len(encoded)} / {MAX_FRAMES} max")

    with open(out_path, "wb") as f:
        f.write(blob)

    print(f"Written: {out_path}  ({len(blob):,} B)")
    print()
    print("Next steps:")
    print(f"  cd {example_dir}")
    print("  python gen_flash.py")
    print("  flash_vape.bat")


if __name__ == "__main__":
    main()
