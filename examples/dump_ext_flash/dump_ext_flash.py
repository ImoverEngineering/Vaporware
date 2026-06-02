#!/usr/bin/env python3
"""dump_ext_flash.py — Dump 1 MB GT25Q80A external flash to original_ext_flash.bin.

Workflow:
  1. Run build_dump.bat first to build build/dump_ext_flash.bin.
  2. Run this script.  It will:
       a. Attach ST-Link to WSL via usbipd
       b. Start OpenOCD in WSL with the TCL server port
       c. Flash dump_ext_flash.bin to internal flash via the TCL port
       d. Reset and run the dump firmware
       e. Read all 512 × 2 KB chunks via SRAM handshake
       f. Save build/original_ext_flash.bin  (1 048 576 bytes)

SRAM handshake addresses (fixed in dump_ext_flash.c):
  0x20000200  status:     0 = busy, 1 = chunk ready, 0xDEADDEAD = done
  0x20000204  chunk_addr: flash address of this chunk
  0x20000400  buf[2048]:  chunk data

The script talks to OpenOCD through its TCL server on localhost:6666.
The CPU is never halted — all reads/writes go through the AHB-AP
(non-halting DAP access), which works fine on Cortex-M0.
"""

import os
import sys
import time
import socket
import struct
import subprocess

# ── Paths ────────────────────────────────────────────────────────────────────
SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
BIN_PATH    = os.path.join(SCRIPT_DIR, "build", "dump_ext_flash.bin")
OUT_PATH    = os.path.join(SCRIPT_DIR, "build", "original_ext_flash.bin")
CFG_WIN     = os.path.join(SCRIPT_DIR, "n32g031.openocd.cfg")

# Convert Windows path to WSL /mnt/c/... path
def win_to_wsl(path):
    drive = path[0].lower()
    rest  = path[2:].replace("\\", "/")
    return f"/mnt/{drive}{rest}"

CFG_WSL = win_to_wsl(CFG_WIN)

# ── SRAM handshake constants (must match dump_ext_flash.c) ───────────────────
STATUS_ADDR     = 0x20000200
CHUNK_ADDR_REG  = 0x20000204
BUF_ADDR        = 0x20000400
FLASH_SIZE      = 0x100000        # 1 MB
CHUNK_SIZE      = 2048
NUM_CHUNKS      = FLASH_SIZE // CHUNK_SIZE   # 512

STATUS_READY    = 1
STATUS_DONE     = 0xDEADDEAD

# ── OpenOCD TCL client ────────────────────────────────────────────────────────
class OpenOCD:
    """Thin wrapper around OpenOCD's TCL server port."""

    def __init__(self, host="localhost", port=6666, default_timeout=30.0):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.s.connect((host, port))
        self.default_timeout = default_timeout

    def cmd(self, command, timeout=None):
        """Send one TCL command, return the response string."""
        if timeout is None:
            timeout = self.default_timeout
        self.s.sendall((command + "\x1a").encode())
        data = b""
        self.s.settimeout(timeout)
        while True:
            try:
                chunk = self.s.recv(65536)
                if not chunk:
                    break
                data += chunk
                if b"\x1a" in data:
                    break
            except socket.timeout:
                break
        return data.decode("utf-8", errors="replace").rstrip("\x1a").strip()

    def mrw(self, addr):
        """Read one 32-bit word (non-halting AHB-AP read)."""
        r = self.cmd(f"mrw 0x{addr:08X}")
        r = r.strip()
        try:
            return int(r, 0)
        except ValueError:
            # Sometimes OpenOCD prefixes extra text; grab last token
            tokens = r.split()
            return int(tokens[-1], 0) if tokens else 0

    def mww(self, addr, val):
        """Write one 32-bit word (non-halting AHB-AP write)."""
        self.cmd(f"mww 0x{addr:08X} 0x{val:08X}")

    def read_memory_bytes(self, addr, count):
        """Read 'count' bytes from 'addr', return bytes object (little-endian words)."""
        n32 = (count + 3) // 4
        r = self.cmd(f"read_memory 0x{addr:08X} 32 {n32}", timeout=60.0)
        words = []
        for tok in r.split():
            tok = tok.strip()
            if tok:
                try:
                    words.append(int(tok, 0))
                except ValueError:
                    pass
        raw = b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in words)
        return raw[:count]

    def halt(self):
        self.cmd("catch {halt}")

    def resume(self):
        self.cmd("resume")

    def close(self):
        self.s.close()


# ── Internal flash programming via TCL (same approach as gen_direct_flash.py) ─
def flash_commands(bin_data):
    """Yield TCL commands that erase and program internal flash with bin_data."""
    # Pad to word boundary
    data = bytearray(bin_data)
    while len(data) % 4:
        data.append(0xFF)
    words  = struct.unpack_from("<" + "I" * (len(data) // 4), bytes(data))
    nbytes = len(data)
    npages = (nbytes + 511) // 512

    yield "adapter speed 500"
    yield "catch {halt}"
    yield "sleep 30"
    yield "catch {halt}"
    yield "sleep 30"

    # Unlock flash
    yield "mww 0x40022004 0x45670123"
    yield "mww 0x40022004 0xCDEF89AB"
    yield "sleep 5"
    yield "mww 0x4002200C 0x00000034"

    # Erase pages (512 B each)
    for i in range(npages):
        pg = 0x08000000 + i * 0x200
        yield f"mww 0x40022014 0x{pg:08X}"
        yield "mww 0x40022010 0x00000042"
        yield "sleep 30"
        yield "mww 0x40022010 0x00000000"
        yield "mww 0x4002200C 0x00000034"

    # Program words
    yield "mww 0x40022010 0x00000001"
    for i, w in enumerate(words):
        addr = 0x08000000 + i * 4
        yield f"mww 0x{addr:08X} 0x{w:08X}"
    yield "mww 0x40022010 0x00000000"
    yield "reset run"


# ── Main ─────────────────────────────────────────────────────────────────────
def main():
    if not os.path.isfile(BIN_PATH):
        print(f"ERROR: {BIN_PATH} not found.")
        print("       Run build_dump.bat first.")
        sys.exit(1)

    with open(BIN_PATH, "rb") as f:
        fw_data = f.read()
    print(f"Firmware : {len(fw_data)} bytes  [{BIN_PATH}]")

    # Attach ST-Link to WSL
    print("Attaching ST-Link to WSL...")
    subprocess.run(["usbipd", "attach", "--wsl", "--busid", "1-2"],
                   capture_output=True)
    time.sleep(2)

    # Start OpenOCD in WSL with TCL server
    ocd_args = [
        "wsl", "openocd",
        "-f", CFG_WSL,
        "-c", "tcl_port 6666; telnet_port disabled; gdb_port disabled",
        "-c", "init",
    ]
    print("Starting OpenOCD in WSL (TCL port 6666)...")
    ocd_proc = subprocess.Popen(
        ocd_args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    # Wait for OpenOCD to be ready
    print("Waiting for OpenOCD to initialise", end="", flush=True)
    ocd = None
    for _ in range(40):
        time.sleep(0.5)
        print(".", end="", flush=True)
        try:
            ocd = OpenOCD("localhost", 6666)
            break
        except ConnectionRefusedError:
            pass
    print()

    if ocd is None:
        print("ERROR: Could not connect to OpenOCD TCL server on port 6666.")
        ocd_proc.terminate()
        sys.exit(1)

    print("Connected to OpenOCD.")

    # Flash dump firmware
    print("Flashing dump firmware...")
    cmds = list(flash_commands(fw_data))
    for i, c in enumerate(cmds):
        ocd.cmd(c, timeout=10.0)
        if i % 50 == 0:
            print(f"  {i}/{len(cmds)}", end="\r", flush=True)
    print(f"  {len(cmds)}/{len(cmds)}  — flash complete.")

    # Firmware now running; give it a moment to start reading first chunk
    time.sleep(0.5)

    # Dump loop
    print(f"Dumping {FLASH_SIZE // 1024} KB in {NUM_CHUNKS} chunks of {CHUNK_SIZE} B...")
    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    all_data = bytearray()

    for chunk_idx in range(NUM_CHUNKS):
        # Poll status (non-halting AHB-AP read; firmware keeps running)
        deadline = time.time() + 10.0  # 10-second timeout per chunk
        while True:
            status = ocd.mrw(STATUS_ADDR)
            if status == STATUS_READY:
                break
            if status == STATUS_DONE:
                print("\nFirmware signalled done early.")
                break
            if time.time() > deadline:
                print(f"\nERROR: timeout waiting for chunk {chunk_idx}")
                ocd.close()
                ocd_proc.terminate()
                sys.exit(1)
            time.sleep(0.02)

        if status == STATUS_DONE:
            break

        # Read 2 KB from SRAM buffer (non-halting)
        chunk = ocd.read_memory_bytes(BUF_ADDR, CHUNK_SIZE)
        all_data.extend(chunk)

        flash_addr = ocd.mrw(CHUNK_ADDR_REG)
        pct = (chunk_idx + 1) * 100 // NUM_CHUNKS
        print(
            f"\r  [{pct:3d}%] chunk {chunk_idx + 1:3d}/{NUM_CHUNKS}"
            f"  flash 0x{flash_addr:06X}  "
            f"({len(all_data) // 1024} KB saved)",
            end="",
            flush=True,
        )

        # Clear status → firmware reads next chunk
        ocd.mww(STATUS_ADDR, 0)

    print()

    # Verify done signal
    time.sleep(0.2)
    final_status = ocd.mrw(STATUS_ADDR)
    if final_status == STATUS_DONE:
        print("Firmware confirmed: all chunks done.")

    # Save output
    with open(OUT_PATH, "wb") as f:
        f.write(all_data)
    print(f"\nSaved: {OUT_PATH}  ({len(all_data):,} bytes)")

    ocd.close()
    ocd_proc.terminate()
    print("Done.  Original external flash content preserved.")


if __name__ == "__main__":
    main()
