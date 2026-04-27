import cv2, time

cap = cv2.VideoCapture(1)
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
time.sleep(2.0)

for _ in range(5):
    ret, frame = cap.read()
cap.release()

if not ret:
    print("FAILED")
    raise SystemExit(1)

cv2.imwrite(r"C:\Users\cooli\Claude_Vapes\cam_frame.bmp", frame)
print("Saved %dx%d, max=%d, mean=%.1f" % (frame.shape[1], frame.shape[0], frame.max(), frame.mean()))

for y in [100, 300, 400, 500, 600]:
    row = []
    for x in [100, 300, 500, 700, 900, 1100]:
        b, g, r = int(frame[y, x, 0]), int(frame[y, x, 1]), int(frame[y, x, 2])
        row.append("(%d,%d,%d)" % (r, g, b))
    print("  y=%d: %s" % (y, "  ".join(row)))

# Calibrated display spot
SPOT_Y0, SPOT_Y1 = 356, 376
SPOT_X0, SPOT_X1 = 565, 595
roi = frame[SPOT_Y0:SPOT_Y1, SPOT_X0:SPOT_X1]
avg_b = float(roi[:, :, 0].mean())
avg_g = float(roi[:, :, 1].mean())
avg_r = float(roi[:, :, 2].mean())
brightness = (avg_r + avg_g + avg_b) / 3
print("Calibrated spot: R=%.0f G=%.0f B=%.0f  brightness=%.0f" % (avg_r, avg_g, avg_b, brightness))
print("Ref working: R=89 G=153 B=249 | Ref black: R=55 G=71 B=67")
if brightness > 120:
    print(">> DISPLAY ACTIVE")
elif brightness > 80:
    print(">> DISPLAY ON (medium)")
else:
    print(">> DISPLAY OFF/BLACK")
