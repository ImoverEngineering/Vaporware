# Capture a single frame from the webcam using VFW (avicap32) via ctypes.
# Analyzes pixel at the calibrated display spot: y=356-376, x=565-595 (in 1280x720)
import ctypes
import ctypes.wintypes
import struct
import sys
import os
import time

# Try using Windows.Media.Capture via subprocess (Windows Camera app trick)
# Fall back to a simple VFW (Video for Windows) approach

def try_vfw_capture(output_path):
    """Use Video for Windows (avicap32.dll) to capture a single frame."""
    try:
        avicap32 = ctypes.windll.avicap32
        user32   = ctypes.windll.user32
        gdi32    = ctypes.windll.gdi32

        WS_CHILD    = 0x40000000
        WS_VISIBLE  = 0x10000000
        WM_CAP_DRIVER_CONNECT      = 0x0400 + 10
        WM_CAP_SET_PREVIEW         = 0x0400 + 50
        WM_CAP_GRAB_FRAME          = 0x0400 + 60
        WM_CAP_EDIT_COPY           = 0x0400 + 30
        WM_CAP_GET_FRAME           = 0x0400 + 60
        WM_CAP_FILE_SAVEDIB        = 0x0400 + 25

        # Create a hidden capture window
        hwnd = avicap32.capCreateCaptureWindowW(
            "cap", WS_CHILD | WS_VISIBLE,
            0, 0, 1280, 720,
            0, 0
        )
        if not hwnd:
            return False, "capCreateCaptureWindowW failed"

        # Connect driver 0
        ret = user32.SendMessageW(hwnd, WM_CAP_DRIVER_CONNECT, 0, 0)
        if not ret:
            user32.DestroyWindow(hwnd)
            return False, "Driver connect failed"

        # Give camera time to initialize
        time.sleep(1.5)

        # Grab a frame
        user32.SendMessageW(hwnd, WM_CAP_GRAB_FRAME, 0, 0)
        time.sleep(0.3)

        # Save as DIB (BMP)
        bmp_path = output_path.replace('.png', '.bmp')
        bmp_path_w = ctypes.create_unicode_buffer(bmp_path)
        ret = user32.SendMessageW(hwnd, WM_CAP_FILE_SAVEDIB, 0, ctypes.addressof(bmp_path_w))

        user32.DestroyWindow(hwnd)

        if os.path.exists(bmp_path) and os.path.getsize(bmp_path) > 1000:
            # Convert BMP to simple analysis — read BMP pixel data
            return True, bmp_path
        return False, f"SaveDIB ret={ret}, file exists={os.path.exists(bmp_path)}"

    except Exception as e:
        return False, str(e)

def read_bmp_pixel(bmp_path, px, py):
    """Read BGR pixel from a BMP file at (px, py)."""
    with open(bmp_path, 'rb') as f:
        data = f.read()
    # BMP header: offset 10 = pixel data start, 18=width, 22=height, 28=bpp
    pix_offset = struct.unpack_from('<I', data, 10)[0]
    width      = struct.unpack_from('<i', data, 18)[0]
    height     = struct.unpack_from('<i', data, 22)[0]
    bpp        = struct.unpack_from('<H', data, 28)[0]
    if bpp != 24:
        return None, f"bpp={bpp} (need 24)"
    row_size = (width * 3 + 3) & ~3
    # BMP rows are stored bottom-up
    row = (abs(height) - 1 - py) if height > 0 else py
    offset = pix_offset + row * row_size + px * 3
    b, g, r = data[offset], data[offset+1], data[offset+2]
    return (r, g, b), None

def analyze_display_region(bmp_path):
    """Sample the calibrated display region y=356-376, x=565-595 in 1280x720."""
    # Scale if BMP is a different resolution
    with open(bmp_path, 'rb') as f:
        data = f.read()
    width  = abs(struct.unpack_from('<i', data, 18)[0])
    height = abs(struct.unpack_from('<i', data, 22)[0])
    print(f"  Frame size: {width}x{height}")

    sx = width  / 1280.0
    sy = height / 720.0

    samples = []
    for py in range(356, 377, 5):
        for px in range(565, 596, 10):
            spx = int(px * sx)
            spy = int(py * sy)
            rgb, err = read_bmp_pixel(bmp_path, spx, spy)
            if rgb:
                samples.append(rgb)

    if not samples:
        print("  No samples read")
        return

    avg_r = sum(s[0] for s in samples) / len(samples)
    avg_g = sum(s[1] for s in samples) / len(samples)
    avg_b = sum(s[2] for s in samples) / len(samples)
    print(f"  Calibrated spot avg: R={avg_r:.0f} G={avg_g:.0f} B={avg_b:.0f}  ({len(samples)} samples)")
    print(f"  Reference (working slotmachine): R=89 G=153 B=249")
    print(f"  Reference (black/off):           R=55 G=71 B=67")
    brightness = (avg_r + avg_g + avg_b) / 3
    print(f"  Average brightness: {brightness:.0f}")
    if brightness > 120:
        print("  >> DISPLAY APPEARS ACTIVE (bright)")
    elif brightness > 80:
        print("  >> DISPLAY MAY BE ON (medium)")
    else:
        print("  >> DISPLAY APPEARS OFF OR BLACK")


if __name__ == "__main__":
    output = r"C:\Users\cooli\Claude_Vapes\cam_frame.bmp"
    print("Capturing webcam frame...")
    ok, result = try_vfw_capture(output)
    if ok:
        print(f"Captured: {result}")
        analyze_display_region(result)
    else:
        print(f"Capture failed: {result}")
        sys.exit(1)
