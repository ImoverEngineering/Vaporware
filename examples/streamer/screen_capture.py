#!/usr/bin/env python3
"""screen_capture.py — Windows-side screen grabber for vape streamer.

Runs on WINDOWS Python (not WSL). Captures a window or the full screen,
scales to 128x160, and writes raw RGB bytes to a shared temp file that
stream_frames.py reads from WSL.

Usage:
    python screen_capture.py                      # capture full screen
    python screen_capture.py "chocolate-doom"     # capture window by title

Requires: pip install pillow
Output file: C:\\temp\\vape_frame.bin  (128*160*3 = 61440 bytes, raw RGB24)
"""

import sys
import time
import os

LCD_W, LCD_H = 128, 160
FRAME_PATH = r'C:\temp\vape_frame.bin'
CAPTURE_FPS = 10   # how many times per second we grab (streaming is ~1.5fps, this keeps it fresh)

os.makedirs(r'C:\temp', exist_ok=True)

try:
    from PIL import ImageGrab, Image
except ImportError:
    print("ERROR: pip install pillow")
    sys.exit(1)

window_title = sys.argv[1] if len(sys.argv) > 1 else None

def get_window_bbox(title):
    """Return (left, top, right, bottom) for a window matching title, or None."""
    try:
        import ctypes
        user32 = ctypes.windll.user32

        found = []
        EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_int, ctypes.c_int)

        def callback(hwnd, _):
            buf = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, buf, 256)
            if title.lower() in buf.value.lower() and user32.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True

        user32.EnumWindows(EnumWindowsProc(callback), 0)
        if not found:
            return None

        hwnd = found[0]
        rect = ctypes.wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        return (rect.left, rect.top, rect.right, rect.bottom)
    except Exception as e:
        print(f"Window find error: {e}")
        return None

print(f"Capturing {'window: ' + window_title if window_title else 'full screen'}")
print(f"Writing to {FRAME_PATH} at {CAPTURE_FPS} fps")
print("Keep this running while stream_frames.py streams to the vape.")
print("Ctrl-C to stop.\n")

frame = 0
interval = 1.0 / CAPTURE_FPS

while True:
    t0 = time.monotonic()

    try:
        if window_title:
            bbox = get_window_bbox(window_title)
            img = ImageGrab.grab(bbox=bbox) if bbox else ImageGrab.grab()
        else:
            img = ImageGrab.grab()

        img = img.convert('RGB').resize((LCD_W, LCD_H), Image.LANCZOS)

        # Atomic write: write to tmp then rename so reader never sees partial file
        tmp = FRAME_PATH + '.tmp'
        with open(tmp, 'wb') as f:
            f.write(img.tobytes())
        os.replace(tmp, FRAME_PATH)

        frame += 1
        if frame % CAPTURE_FPS == 0:
            print(f"\r  Captured {frame} frames", end='', flush=True)

    except Exception as e:
        print(f"\nCapture error: {e}")

    elapsed = time.monotonic() - t0
    wait = interval - elapsed
    if wait > 0:
        time.sleep(wait)
