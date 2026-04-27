import cv2, sys, time

CAM_IDX = 0
OUT_BMP = r"C:\Users\cooli\Claude_Vapes\cam_frame.bmp"

# Calibrated display spot in 1280x720 frame
SPOT_Y0, SPOT_Y1 = 356, 376
SPOT_X0, SPOT_X1 = 565, 595

cap = cv2.VideoCapture(CAM_IDX, cv2.CAP_DSHOW)
cap.set(cv2.CAP_PROP_FRAME_WIDTH,  1280)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)

time.sleep(1.0)   # let camera settle

ok = False
for _ in range(10):
    ret, frame = cap.read()
    if ret:
        ok = True
        break
    time.sleep(0.1)

cap.release()

if not ok:
    print("ERROR: could not read frame from camera")
    sys.exit(1)

h, w = frame.shape[:2]
print(f"Frame: {w}x{h}")
cv2.imwrite(OUT_BMP, frame)
print(f"Saved: {OUT_BMP}")

# Scale calibration coords to actual frame size
sx = w / 1280.0
sy = h / 720.0
x0 = int(SPOT_X0 * sx); x1 = int(SPOT_X1 * sx)
y0 = int(SPOT_Y0 * sy); y1 = int(SPOT_Y1 * sy)

roi = frame[y0:y1, x0:x1]   # BGR
avg_b = float(roi[:,:,0].mean())
avg_g = float(roi[:,:,1].mean())
avg_r = float(roi[:,:,2].mean())
brightness = (avg_r + avg_g + avg_b) / 3

print(f"Display spot ({x0},{y0})-({x1},{y1}): R={avg_r:.0f} G={avg_g:.0f} B={avg_b:.0f}")
print(f"Brightness: {brightness:.0f}")
print(f"Ref working slots: R=89 G=153 B=249  |  Ref black: R=55 G=71 B=67")

if brightness > 120:
    print(">> DISPLAY ACTIVE (bright)")
elif brightness > 80:
    print(">> DISPLAY ON (medium brightness)")
else:
    print(">> DISPLAY OFF or BLACK")
