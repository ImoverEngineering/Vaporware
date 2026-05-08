#!/usr/bin/env python3
"""diag_timing.py — per-step timing breakdown for SWD streaming.

Connects, sends 5 chunks with verbose timing, prints breakdown.
"""
import re, socket, struct, subprocess, sys, time

OCD_TARGET_CFG = (
    "/mnt/c/Users/cooli/Claude_Vapes/Vaporware/examples/streamer/"
    "n32g031.openocd.cfg"
)
USBIPD_BUSID    = "1-2"
OCD_TELNET_PORT = 4444

FAST_IDX_ADDR  = 0x20000100
FAST_BUF_ADDR  = 0x20000104
FAST_TRIG_ADDR = 0x20001104
CTRL_CHUNK     = 0x000000CC

WSL_CHUNK_PATH = '/tmp/vape_chunk.bin'
NUM_TESTS      = 5

_ANSI_RE = re.compile(rb'\x1b\[[^m]*m')


def ocd_connect():
    print("Starting WSL keepalive…")
    subprocess.run(['wsl', 'echo', 'ready'], capture_output=True, timeout=10)
    keepalive = subprocess.Popen(
        ['wsl', '-u', 'root', 'bash', '-c', 'sleep infinity'],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.3)

    print("Launching sidecar…")
    _sidecar = (
        'import sys\np="/tmp/vape_chunk.bin"\n'
        'while True:\n  d=sys.stdin.buffer.read(4096)\n  if not d: break\n'
        '  open(p,"wb").write(d)\n  sys.stdout.write("ok\\n"); sys.stdout.flush()\n'
    )
    writer = subprocess.Popen(
        ['wsl', '-u', 'root', 'python3', '-c', _sidecar],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    print("Attaching ST-Link…")
    subprocess.run(['usbipd', 'attach', '--wsl', '--busid', USBIPD_BUSID],
                   capture_output=True, text=True)
    time.sleep(1.5)

    print("Starting OpenOCD…")
    ocd = subprocess.Popen(
        ['wsl', '-u', 'root', 'bash', '-c',
         f'openocd -f {OCD_TARGET_CFG} -c "init" -c "reset halt"'],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(5.0)
    if ocd.poll() is not None:
        raise RuntimeError(f"OpenOCD exited: rc={ocd.poll()}")

    sock = socket.create_connection(('localhost', OCD_TELNET_PORT), timeout=5)
    sock.settimeout(15)
    return sock, ocd, keepalive, writer


rxbuf = b''

def read_prompt(sock, timeout=10.0):
    global rxbuf
    deadline = time.monotonic() + timeout
    sock.settimeout(0.05)
    try:
        while True:
            s = _ANSI_RE.sub(b'', rxbuf)
            i = s.find(b'>')
            if i >= 0:
                resp = s[:i+1].decode('utf-8', errors='replace')
                rxbuf = s[i+1:]
                return resp
            if time.monotonic() >= deadline:
                break
            try:
                d = sock.recv(4096)
                if not d: break
                rxbuf += d
            except socket.timeout:
                pass
    finally:
        sock.settimeout(15)
    result = _ANSI_RE.sub(b'', rxbuf).decode('utf-8', errors='replace')
    rxbuf = b''
    return result

def cmd(sock, command, timeout=15.0):
    sock.sendall((command + '\n').encode())
    return read_prompt(sock, timeout=timeout)


# Build a solid green chunk
GREEN_BGR565 = 0x07E0
_chunk = struct.pack('<' + 'H' * (128 * 16), *([GREEN_BGR565] * (128 * 16)))


def timed_send_chunk(sock, writer, idx, freq_label):
    """Send one chunk, print timing for each phase."""
    times = {}

    # 1. Sidecar write
    t = time.monotonic()
    writer.stdin.write(_chunk)
    writer.stdin.flush()
    writer.stdout.readline()
    times['sidecar_ms'] = (time.monotonic() - t) * 1000

    # 2. mww IDX
    t = time.monotonic()
    cmd(sock, f'mww 0x{FAST_IDX_ADDR:08X} 0x{idx:08X}')
    times['mww_idx_ms'] = (time.monotonic() - t) * 1000

    # 3. load_image
    t = time.monotonic()
    cmd(sock, f'load_image {WSL_CHUNK_PATH} 0x{FAST_BUF_ADDR:08X} bin', timeout=30)
    times['load_image_ms'] = (time.monotonic() - t) * 1000

    # 4. mww TRIG
    t = time.monotonic()
    cmd(sock, f'mww 0x{FAST_TRIG_ADDR:08X} 0x{CTRL_CHUNK:08X}')
    times['mww_trig_ms'] = (time.monotonic() - t) * 1000

    # 5. Wait for MCU to clear TRIG (draw complete)
    t = time.monotonic()
    polls = 0
    deadline = time.monotonic() + 0.200
    while time.monotonic() < deadline:
        resp = cmd(sock, f'mrw 0x{FAST_TRIG_ADDR:08X}')
        polls += 1
        if '0xcc' not in resp.lower():
            break
    times['wait_trig_ms'] = (time.monotonic() - t) * 1000
    times['polls'] = polls

    total = sum(v for k, v in times.items() if k != 'polls')
    print(f"  [{freq_label}] chunk {idx}:  "
          f"sidecar={times['sidecar_ms']:.1f}ms  "
          f"mww_idx={times['mww_idx_ms']:.1f}ms  "
          f"load_image={times['load_image_ms']:.1f}ms  "
          f"mww_trig={times['mww_trig_ms']:.1f}ms  "
          f"wait_trig={times['wait_trig_ms']:.1f}ms ({polls} polls)  "
          f"TOTAL={total:.1f}ms")
    return times


def main():
    sock, ocd, keepalive, writer = ocd_connect()
    read_prompt(sock)   # welcome

    for freq_khz in [2000, 4000, 8000, 12000]:
        print(f"\n=== {freq_khz} kHz SWD ===")
        cmd(sock, f'adapter speed {freq_khz}')
        cmd(sock, 'resume')
        time.sleep(1.5)  # let firmware show draw_waiting

        all_times = []
        for i in range(NUM_TESTS):
            t = timed_send_chunk(sock, writer, i % 10, f'{freq_khz}kHz')
            all_times.append(t)

        avg = {k: sum(d[k] for d in all_times) / len(all_times)
               for k in all_times[0] if k != 'polls'}
        avg_polls = sum(d['polls'] for d in all_times) / len(all_times)
        total_avg = sum(avg[k] for k in avg)
        print(f"  AVERAGES: sidecar={avg['sidecar_ms']:.1f}  "
              f"mww_idx={avg['mww_idx_ms']:.1f}  "
              f"load_image={avg['load_image_ms']:.1f}  "
              f"mww_trig={avg['mww_trig_ms']:.1f}  "
              f"wait_trig={avg['wait_trig_ms']:.1f} ({avg_polls:.1f} polls)  "
              f"TOTAL={total_avg:.1f}ms  → {1000/total_avg:.2f} chunks/s")

        cmd(sock, 'halt')

    print("\nShutting down.")
    try: sock.send(b'shutdown\n')
    except: pass
    sock.close()
    try: ocd.wait(timeout=3)
    except: ocd.terminate()
    writer.stdin.close()
    keepalive.terminate()


if __name__ == '__main__':
    main()
