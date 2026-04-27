"""backlight_toggle_test.py
Halt MCU, toggle PB4 (backlight) 6 times, capture camera frame each time.
Compute which region changes in sync with PB4 toggle.
"""
import socket, time
import cv2
import numpy as np

s = socket.socket()
s.settimeout(10.0)
s.connect(('localhost', 6666))

def cmd(c):
    s.sendall((c+'\x1a').encode())
    buf = b''
    t0=time.time()
    while not buf.endswith(b'\x1a'):
        try:
            s.settimeout(2.0)
            chunk = s.recv(4096)
            if not chunk: break
            buf += chunk
        except:
            if time.time()-t0>4: break
    return buf[:-1].decode(errors='replace').strip()

def mww(addr, val): cmd('mww 0x%08X 0x%08X' % (addr, val))

GPIOB_BSRR = 0x40010C18
BL_HIGH = 1 << 4          # BS4 = PB4 HIGH = backlight OFF
BL_LOW  = 1 << (4 + 16)   # BR4 = PB4 LOW  = backlight ON

BASE = r'C:\Users\cooli\Claude_Vapes'

cap = cv2.VideoCapture(1, cv2.CAP_DSHOW)
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
time.sleep(1.5)
for _ in range(20): cap.read()

cmd('halt')
time.sleep(0.1)

def snap():
    for _ in range(3): cap.read()
    ret, f = cap.read()
    return f if ret else None

frames_on = []
frames_off = []

print('Toggling backlight 6 times, 1s each...')
for i in range(6):
    if i % 2 == 0:
        mww(GPIOB_BSRR, BL_LOW)   # backlight ON
        state = 'ON '
    else:
        mww(GPIOB_BSRR, BL_HIGH)  # backlight OFF
        state = 'OFF'
    time.sleep(0.8)
    f = snap()
    if f is not None:
        gray = cv2.cvtColor(f, cv2.COLOR_BGR2GRAY)
        print(f'  BL={state}: frame mean={gray.mean():.1f}')
        if i % 2 == 0:
            frames_on.append(f.astype(np.float32))
        else:
            frames_off.append(f.astype(np.float32))

# Turn backlight back ON, resume
mww(GPIOB_BSRR, BL_LOW)
cmd('resume')
s.close()
cap.release()

if not frames_on or not frames_off:
    print('Not enough frames'); raise SystemExit(1)

on_avg  = np.mean(frames_on,  axis=0)
off_avg = np.mean(frames_off, axis=0)
diff = on_avg.mean(axis=2) - off_avg.mean(axis=2)  # positive = brighter when BL on

print(f'\nDiff: mean={diff.mean():.2f}  max={diff.max():.2f}  min={diff.min():.2f}')

# Where is the display?
smooth = cv2.GaussianBlur(np.abs(diff).astype(np.float32), (21,21), 0)
y_pk, x_pk = np.unravel_index(smooth.argmax(), smooth.shape)
print(f'Peak BL-toggle response at ({x_pk},{y_pk}) = {smooth[y_pk,x_pk]:.2f}')

# Grid
print('\nBL-difference grid (positive = brighter when BL ON):')
for gy in range(0, 720, 90):
    row = []
    for gx in range(0, 1280, 160):
        v = diff[gy:gy+90, gx:gx+160].mean()
        row.append('%+5.1f' % v)
    print(f'  y={gy:3d}: {"  ".join(row)}')

# Annotate
last = off_avg.astype(np.uint8).copy()
cv2.circle(last, (x_pk, y_pk), 25, (0,0,255), 3)
cv2.putText(last, f'BL peak ({x_pk},{y_pk})', (max(0,x_pk-80), max(20,y_pk-30)),
            cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0,0,255), 2)

# Find bbox
thresh = max(smooth.max()*0.3, 3.0)
mask = (smooth > thresh).astype(np.uint8)*255
contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
if contours:
    best = max(contours, key=cv2.contourArea)
    x,y,w,h = cv2.boundingRect(best)
    cv2.rectangle(last, (x,y), (x+w,y+h), (0,255,0), 3)
    cv2.putText(last, 'BACKLIGHT?', (x, max(10,y-10)),
                cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0,255,0), 2)
    print(f'\nBacklight-sensitive bbox: ({x},{y})-({x+w},{y+h}) size={w}x{h}px')

cv2.imwrite(f'{BASE}/bl_toggle_result.jpg', last)
print('Saved bl_toggle_result.jpg')

if smooth.max() > 5:
    print('\n>> BACKLIGHT IS RESPONDING to PB4 toggle!')
else:
    print('\n>> NO response to PB4 toggle — backlight wire may be disconnected!')
