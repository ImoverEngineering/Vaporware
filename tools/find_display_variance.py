"""find_display_variance.py
Take 12 snapshots over 24s while doom_v2 cycles colors.
Compute temporal variance to find which region changes (= display area).
"""
import cv2
import numpy as np
import time

cap = cv2.VideoCapture(1, cv2.CAP_DSHOW)
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
time.sleep(1.0)
for _ in range(20): cap.read()

BASE = r'C:\Users\cooli\Claude_Vapes'
frames = []

print("Collecting 12 frames over 24s...")
for i in range(12):
    for _ in range(3): cap.read()
    ret, f = cap.read()
    if ret:
        frames.append(f.astype(np.float32))
        gray = cv2.cvtColor(f, cv2.COLOR_BGR2GRAY)
        print(f"  frame {i+1}/12: mean={gray.mean():.1f}")
    time.sleep(2.0)

cap.release()

if len(frames) < 2:
    print("Not enough frames")
    raise SystemExit(1)

stack = np.stack(frames, axis=0)  # shape: (N, H, W, 3)
var = stack.var(axis=0)           # (H, W, 3) temporal variance per pixel
var_gray = var.mean(axis=2)       # (H, W) average channel variance

print(f"\nGlobal variance: mean={var_gray.mean():.2f} max={var_gray.max():.2f}")

# Find the region with highest variance (that's the display)
smooth = cv2.GaussianBlur(var_gray.astype(np.float32), (21, 21), 0)
y_pk, x_pk = np.unravel_index(smooth.argmax(), smooth.shape)
print(f"Peak variance at ({x_pk}, {y_pk}) = {smooth[y_pk, x_pk]:.2f}")

# Show a grid of variance values
print("\nVariance grid (8x8 blocks):")
for gy in range(0, 720, 90):
    row = []
    for gx in range(0, 1280, 160):
        v = var_gray[gy:gy+90, gx:gx+160].mean()
        row.append('%5.1f' % v)
    print(f"  y={gy:3d}: {'  '.join(row)}")

# Annotate the last frame with the high-variance region
last = frames[-1].astype(np.uint8).copy()
thresh = max(smooth.max() * 0.25, 5.0)
mask = (smooth > thresh).astype(np.uint8) * 255
contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
if contours:
    best = max(contours, key=cv2.contourArea)
    x, y, w, h = cv2.boundingRect(best)
    print(f"\nHigh-variance bbox: ({x},{y})-({x+w},{y+h}) size={w}x{h}px")
    cv2.rectangle(last, (x, y), (x+w, y+h), (0, 255, 0), 3)
    cv2.putText(last, "DISPLAY?", (x, y-10), cv2.FONT_HERSHEY_SIMPLEX, 1, (0,255,0), 2)

# Mark peak
cv2.circle(last, (x_pk, y_pk), 20, (0, 0, 255), 3)
cv2.imwrite(f'{BASE}/display_variance.jpg', last)

# Save variance heatmap
heatmap = (var_gray / max(var_gray.max(), 1.0) * 255).astype(np.uint8)
heatmap_color = cv2.applyColorMap(heatmap, cv2.COLORMAP_JET)
cv2.imwrite(f'{BASE}/display_heatmap.jpg', heatmap_color)

print(f"\nSaved display_variance.jpg and display_heatmap.jpg")
if smooth.max() > 20:
    print(">> HIGH variance detected — display IS cycling colors!")
elif smooth.max() > 5:
    print(">> LOW variance — display may be on but showing static content")
else:
    print(">> NEGLIGIBLE variance — display appears OFF or static")
