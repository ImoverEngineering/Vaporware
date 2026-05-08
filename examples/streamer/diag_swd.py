#!/usr/bin/env python3
"""diag_swd.py — SWD write diagnostic for the N32G031 streamer.

Tests whether write_memory can reliably reach FAST_TRIG (0x20001104)
while the MCU is running, and whether the MCU firmware detects it.

Steps:
  1. Connect OpenOCD (reset halt + resume, let MCU show draw_waiting).
  2. HALT the MCU.
  3. Write a solid green frame to FAST_BUF via write_memory (while halted).
  4. Write FAST_IDX = 0, FAST_TRIG = 0xCC via mww (while halted).
  5. Read back FAST_TRIG to confirm the write landed.
  6. RESUME the MCU.  It should detect FAST_TRIG=0xCC immediately and draw
     the green frame — replacing draw_waiting().
  7. If screen turns green: firmware loop is fine, SWD-while-running is the issue.
     If screen stays magenta: firmware loop is broken.

Usage: python diag_swd.py
"""

import re
import socket
import struct
import subprocess
import sys
import time

OCD_TARGET_CFG = (
    "/mnt/c/Users/cooli/Claude_Vapes/Vaporware/examples/streamer/"
    "n32g031.openocd.cfg"
)
USBIPD_BUSID   = "1-2"
OCD_TELNET_PORT = 4444

FAST_IDX_ADDR  = 0x20000100
FAST_BUF_ADDR  = 0x20000104
FAST_TRIG_ADDR = 0x20001104
CTRL_CHUNK     = 0x000000CC

LCD_W, LCD_H   = 128, 160
CHUNK_ROWS     = 16
NUM_CHUNKS     = 10

_ANSI_RE = re.compile(rb'\x1b\[[^m]*m')


def openocd_connect():
    """Start OpenOCD in WSL, return telnet socket."""
    print("  Waking WSL…")
    subprocess.run(['wsl', 'echo', 'ready'], capture_output=True, timeout=10)

    wsl_keepalive = subprocess.Popen(
        ['wsl', '-u', 'root', 'bash', '-c', 'sleep infinity'],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(0.5)

    print("  Attaching ST-Link to WSL…")
    subprocess.run(
        ['usbipd', 'attach', '--wsl', '--busid', USBIPD_BUSID],
        capture_output=True, text=True
    )
    time.sleep(1.5)

    print("  Starting OpenOCD…")
    ocd_cmd = f'openocd -f {OCD_TARGET_CFG} -c "init" -c "reset halt"'
    ocd_proc = subprocess.Popen(
        ['wsl', '-u', 'root', 'bash', '-c', ocd_cmd],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(5.0)

    if ocd_proc.poll() is not None:
        raise RuntimeError(f"OpenOCD exited early (rc={ocd_proc.poll()})")

    sock = socket.create_connection(('localhost', OCD_TELNET_PORT), timeout=5)
    sock.settimeout(15)
    return sock, ocd_proc, wsl_keepalive


def read_prompt(sock, rxbuf, timeout=10.0):
    deadline = time.monotonic() + timeout
    sock.settimeout(0.05)
    try:
        while True:
            stripped = _ANSI_RE.sub(b'', rxbuf)
            idx = stripped.find(b'>')
            if idx >= 0:
                resp = stripped[:idx + 1].decode('utf-8', errors='replace')
                rxbuf = stripped[idx + 1:]
                return resp, rxbuf
            if time.monotonic() >= deadline:
                break
            try:
                data = sock.recv(4096)
                if not data:
                    break
                rxbuf += data
            except socket.timeout:
                pass
    finally:
        sock.settimeout(15)
    return _ANSI_RE.sub(b'', rxbuf).decode('utf-8', errors='replace'), b''


def cmd(sock, rxbuf, command, timeout=15.0):
    sock.sendall((command + '\n').encode())
    resp, rxbuf = read_prompt(sock, rxbuf, timeout=timeout)
    return resp, rxbuf


def main():
    sock, ocd_proc, keepalive = openocd_connect()
    rxbuf = b''

    # Consume welcome banner
    _, rxbuf = read_prompt(sock, rxbuf)

    print("  Setting adapter speed 4000 kHz…")
    _, rxbuf = cmd(sock, rxbuf, 'adapter speed 4000')

    # Resume MCU — let it run through init (~265ms) and show draw_waiting()
    print("  Resuming MCU (waiting 2s for draw_waiting to appear)…")
    _, rxbuf = cmd(sock, rxbuf, 'resume')
    time.sleep(2.0)

    print("\n=== PHASE 1: HALT + write while halted ===")
    print("  Halting MCU…")
    _, rxbuf = cmd(sock, rxbuf, 'halt')

    # Read FAST_TRIG before writing
    resp, rxbuf = cmd(sock, rxbuf, f'mrw 0x{FAST_TRIG_ADDR:08X}')
    print(f"  FAST_TRIG before write: {resp.strip()}")

    # Build a solid green frame (all 10 chunks = 128×160 pixels)
    # COL_RGB(0,255,0) with BGR swap: B=0, G=255, R=0
    # BGR565 = (0>>3)<<11 | (255>>2)<<5 | (0>>3) = 0 | 63<<5 | 0 = 0x07E0
    GREEN_BGR565 = 0x07E0
    chunk_words  = [GREEN_BGR565 | (GREEN_BGR565 << 16)] * (128 * 16 // 2)  # 2 px/word

    print("  Writing green frame (halted) chunk 0 to FAST_BUF…")
    word_str = ' '.join(f'0x{w:08X}' for w in ([0] + chunk_words + [CTRL_CHUNK]))
    wm_cmd = f'write_memory 0x{FAST_IDX_ADDR:08X} 32 {{{word_str}}}'
    resp, rxbuf = cmd(sock, rxbuf, wm_cmd, timeout=30)
    print(f"  write_memory response: '{resp.strip()}'")

    # Read back FAST_TRIG — should be 0xCC
    resp, rxbuf = cmd(sock, rxbuf, f'mrw 0x{FAST_TRIG_ADDR:08X}')
    print(f"  FAST_TRIG after write_memory: {resp.strip()}")

    # Also write FAST_IDX = 0 and FAST_TRIG = 0xCC explicitly via mww
    _, rxbuf = cmd(sock, rxbuf, f'mww 0x{FAST_IDX_ADDR:08X} 0x00000000')
    _, rxbuf = cmd(sock, rxbuf, f'mww 0x{FAST_TRIG_ADDR:08X} 0x{CTRL_CHUNK:08X}')
    resp, rxbuf = cmd(sock, rxbuf, f'mrw 0x{FAST_TRIG_ADDR:08X}')
    print(f"  FAST_TRIG after mww:          {resp.strip()}")

    print("\n  Resuming MCU — it should detect FAST_TRIG=0xCC immediately.")
    print("  WATCH THE DISPLAY: rows 0-15 should turn GREEN.")
    _, rxbuf = cmd(sock, rxbuf, 'resume')
    time.sleep(0.5)

    # Check if MCU cleared FAST_TRIG (means it processed it)
    _, rxbuf = cmd(sock, rxbuf, 'halt')
    resp, rxbuf = cmd(sock, rxbuf, f'mrw 0x{FAST_TRIG_ADDR:08X}')
    print(f"  FAST_TRIG 500ms after resume: {resp.strip()}")
    print("  (0x00 = MCU processed it; 0xCC = MCU never saw it)")

    # Resume again for normal operation
    _, rxbuf = cmd(sock, rxbuf, 'resume')

    print("\n=== PHASE 2: write_memory WHILE RUNNING ===")
    time.sleep(0.3)

    # Now try write_memory to chunk 1 while MCU is running
    print("  Writing green chunk 1 via write_memory while MCU is running…")
    word_str = ' '.join(f'0x{w:08X}' for w in ([1] + chunk_words + [CTRL_CHUNK]))
    wm_cmd = f'write_memory 0x{FAST_IDX_ADDR:08X} 32 {{{word_str}}}'
    resp, rxbuf = cmd(sock, rxbuf, wm_cmd, timeout=30)
    print(f"  write_memory response: '{resp.strip()}'")

    # Halt and check FAST_TRIG
    _, rxbuf = cmd(sock, rxbuf, 'halt')
    resp_trig, rxbuf = cmd(sock, rxbuf, f'mrw 0x{FAST_TRIG_ADDR:08X}')
    resp_idx,  rxbuf = cmd(sock, rxbuf, f'mrw 0x{FAST_IDX_ADDR:08X}')
    print(f"  FAST_TRIG after running write_memory: {resp_trig.strip()}")
    print(f"  FAST_IDX  after running write_memory: {resp_idx.strip()}")
    print("  (0x00 for TRIG = MCU processed; 0xCC = unprocessed; other = wrong write)")

    _, rxbuf = cmd(sock, rxbuf, 'resume')

    print("\nDone.  Shutting down OpenOCD.")
    try:
        sock.send(b'shutdown\n')
    except Exception:
        pass
    sock.close()
    try:
        ocd_proc.wait(timeout=3)
    except Exception:
        ocd_proc.terminate()
    keepalive.terminate()


if __name__ == '__main__':
    main()
