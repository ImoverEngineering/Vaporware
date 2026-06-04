#!/usr/bin/env python3
"""gen_flash.py — Generate build/combined_flash.tcl for the ext-flash flash_video example.

Reads:
  build/ext_flash_writer.bin  — writer utility firmware
  build/ext_flash_video.bin   — video blob for external flash
  build/flash_video.bin       — playback firmware

Outputs:
  build/combined_flash.tcl

Phase 1: Flash ext_flash_writer to internal flash (mww, 500 kHz adapter)
Phase 2: Writer runs at 48 MHz; host writes video to ext flash via SRAM handshake
         (adapter speed 4000 for faster SWD data transfer)
Phase 3: Flash flash_video firmware to internal flash (mww, 500 kHz adapter)
         Then reset run — vape plays the video.

Write time estimate (Phase 2):
  ~3 ms per page × n_pages + ~100 ms per sector × n_sectors + JTAG overhead
  A 10-frame clip (~400 KB, ~25 sectors) takes roughly 2–3 minutes.
"""

import os
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

WRITER_BIN  = os.path.join(SCRIPT_DIR, "build", "ext_flash_writer.bin")
VIDEO_BIN   = os.path.join(SCRIPT_DIR, "build", "ext_flash_video.bin")
PLAYER_BIN  = os.path.join(SCRIPT_DIR, "build", "flash_video.bin")
OUT_TCL     = os.path.join(SCRIPT_DIR, "build", "combined_flash.tcl")

# SRAM handshake addresses (must match ext_flash_writer.c)
CMD_ADDR    = 0x20000200
ADDR_ADDR   = 0x20000204
STATUS_ADDR = 0x20000208
BUF_ADDR    = 0x20000400

PAGE_SIZE   = 256
SECTOR_SIZE = 4096


def pad4(data: bytes) -> bytes:
    while len(data) % 4:
        data += b"\xff"
    return data


def gen_internal_flash_tcl(bin_data: bytes, label: str) -> list:
    """Generate TCL lines to erase+program internal flash with bin_data."""
    data   = pad4(bin_data)
    words  = struct.unpack_from("<" + "I" * (len(data) // 4), data)
    npages = (len(data) + 511) // 512

    lines = []
    lines.append(f"# === {label}: program internal flash ({len(bin_data)} B, {npages} pages) ===")
    lines.append("adapter speed 1000")
    lines.append("catch {halt}")
    lines.append("sleep 20")
    lines.append("catch {halt}")
    lines.append("sleep 20")
    lines.append("catch {halt}")
    lines.append("sleep 30")

    # Kick IWDG immediately, then extend to ~17.5 s (PR=6 /256, RLR=4095)
    lines.append("mww 0x40003000 0x0000AAAA")   # kick
    lines.append("sleep 5")
    lines.append("mww 0x40003000 0x00005555")   # unlock PR/RLR
    lines.append("sleep 2")
    lines.append("mww 0x40003004 0x00000006")   # PR=6 -> /256
    lines.append("sleep 2")
    lines.append("mww 0x40003008 0x00000FFF")   # RLR=4095
    lines.append("sleep 2")
    lines.append("mww 0x40003000 0x0000AAAA")   # kick with new settings
    lines.append("sleep 5")

    # Unlock flash
    lines.append("mww 0x40022004 0x45670123")
    lines.append("mww 0x40022004 0xCDEF89AB")
    lines.append("sleep 5")
    lines.append("mww 0x4002200C 0x00000034")

    # Erase pages (two-step: set PER, then STRT; kick IWDG before each)
    for i in range(npages):
        pg = 0x08000000 + i * 0x200
        lines.append("mww 0x40003000 0x0000AAAA")   # IWDG kick before each erase
        lines.append(f"mww 0x40022014 0x{pg:08X}")
        lines.append("mww 0x40022010 0x00000002")   # PER
        lines.append("mww 0x40022010 0x00000042")   # PER + STRT
        lines.append("sleep 30")
        lines.append("mww 0x40022010 0x00000000")
        lines.append("mww 0x4002200C 0x00000034")

    # Program (kick IWDG every 30 words)
    lines.append("mww 0x40003000 0x0000AAAA")       # IWDG kick before write loop
    lines.append("mww 0x40022010 0x00000001")
    for i, w in enumerate(words):
        if i % 30 == 0 and i > 0:
            lines.append("mww 0x40003000 0x0000AAAA")   # IWDG kick every 30 words
        addr = 0x08000000 + i * 4
        lines.append(f"mww 0x{addr:08X} 0x{w:08X}")
    lines.append("sleep 5")
    lines.append("mww 0x40022010 0x00000000")

    # Verify: read back first two flash words to confirm write succeeded
    lines.append("set _f0 [mrw 0x08000000]")
    lines.append("set _f4 [mrw 0x08000004]")
    w0 = words[0]
    w1 = words[1] if len(words) > 1 else 0xFFFFFFFF
    lines.append(f'puts "{label} verify: \\[0]=0x[format %08X $_f0] \\[4]=0x[format %08X $_f4]"')
    lines.append(f'puts "  (expect   \\[0]=0x{w0:08X}  \\[4]=0x{w1:08X})"')
    lines.append(f'if {{$_f0 != 0x{w0:08X}}} {{ error "{label}: flash write failed (word 0 mismatch)" }}')

    return lines


def gen_ext_flash_write_tcl(video_data: bytes) -> list:
    """Generate TCL lines for Phase 2: write video_data to external flash.

    Uses fixed sleeps instead of mrw-based STATUS polling, which is unreliable
    on hla_swd (ST-Link V2) while the target is running.
    Fixed delays: 600 ms per sector erase, 10 ms per page program.
    A diagnostic halt after boot verifies the writer firmware was programmed.
    """
    n_bytes   = len(video_data)
    n_pages   = (n_bytes + PAGE_SIZE  - 1) // PAGE_SIZE
    n_sectors = (n_bytes + SECTOR_SIZE - 1) // SECTOR_SIZE

    lines = []
    lines.append(f"# === Phase 2: write {n_bytes} B of video to external flash ===")
    lines.append(f"# {n_sectors} sectors to erase, {n_pages} pages to program")
    lines.append(f"# Fixed sleeps: 600 ms/sector erase, 10 ms/page program")
    lines.append("")

    # ── Boot diagnostic ────────────────────────────────────────────────────────
    # Already slept 800 ms in phase 1 reset run.  Sleep another 1200 ms so the
    # writer has had ≥2 s to boot, boost PLL, and set STATUS = 1.
    lines.append("# Diagnostic halt: verify writer firmware is running")
    lines.append("sleep 1200")
    lines.append("catch {halt}")
    lines.append("sleep 50")
    lines.append("catch {halt}")
    lines.append("sleep 50")
    lines.append("set _sp  [mrw 0x08000000]")
    lines.append("set _rv  [mrw 0x08000004]")
    lines.append(f"set _st  [mrw 0x{STATUS_ADDR:08X}]")
    lines.append(f"set _cmd [mrw 0x{CMD_ADDR:08X}]")
    lines.append('puts "Writer SP=0x[format %08X $_sp] ResetVec=0x[format %08X $_rv] STATUS=$_st CMD=$_cmd"')
    lines.append('puts "  (expect SP=0x20000800  ResetVec~0x080000B5  STATUS=1  CMD=0)"')
    lines.append("if {$_sp != 0x20000800} {")
    lines.append('    error "Writer firmware NOT programmed (SP=0x[format %08X $_sp]). Check flash erase/write."')
    lines.append("}")
    lines.append("if {$_st != 1} {")
    lines.append('    puts "WARNING: STATUS=$_st (not 1). Waiting another 2 s..."')
    lines.append("    resume")
    lines.append("    sleep 2000")
    lines.append("    catch {halt}")
    lines.append("    sleep 50")
    lines.append("    catch {halt}")
    lines.append(f"    set _st [mrw 0x{STATUS_ADDR:08X}]")
    lines.append('    puts "STATUS after extra wait: $_st"')
    lines.append("    if {$_st != 1} {")
    lines.append('        error "Writer firmware not signaling ready (STATUS=$_st)"')
    lines.append("    }")
    lines.append("}")
    lines.append("resume")
    lines.append("")

    current_sector = -1

    for page_idx in range(n_pages):
        flash_addr  = page_idx * PAGE_SIZE
        sector_addr = (flash_addr // SECTOR_SIZE) * SECTOR_SIZE

        if sector_addr != current_sector:
            lines.append(f"# --- Sector 0x{sector_addr:06X} ---")
            lines.append(f"mww 0x{ADDR_ADDR:08X} 0x{sector_addr:08X}")
            lines.append(f"mww 0x{CMD_ADDR:08X} 0x00000001")   # cmd=erase
            lines.append("sleep 600")                           # GT25Q80A max erase ~400 ms
            current_sector = sector_addr

        # Write 256-byte page to SRAM buffer (64 × mww)
        start  = page_idx * PAGE_SIZE
        end    = min(start + PAGE_SIZE, n_bytes)
        page   = bytearray(video_data[start:end])
        while len(page) < PAGE_SIZE:
            page.append(0xFF)   # pad last page

        lines.append(f"# Page {page_idx} (flash 0x{flash_addr:06X})")
        for w_idx in range(PAGE_SIZE // 4):
            word      = struct.unpack_from("<I", page, w_idx * 4)[0]
            sram_addr = BUF_ADDR + w_idx * 4
            lines.append(f"mww 0x{sram_addr:08X} 0x{word:08X}")

        lines.append(f"mww 0x{ADDR_ADDR:08X} 0x{flash_addr:08X}")
        lines.append(f"mww 0x{CMD_ADDR:08X} 0x00000002")   # cmd=program
        lines.append("sleep 10")                            # GT25Q80A max program ~3 ms

        if page_idx % 16 == 0:
            lines.append(f'puts "Page {page_idx} / {n_pages}"')

    lines.append(f"mww 0x{CMD_ADDR:08X} 0xDEADDEAD")      # cmd=done
    lines.append(f'puts "External flash write complete."')
    lines.append("")

    return lines


def main():
    for path, name in [(WRITER_BIN, "ext_flash_writer.bin"),
                       (VIDEO_BIN,  "ext_flash_video.bin"),
                       (PLAYER_BIN, "flash_video.bin")]:
        if not os.path.isfile(path):
            print(f"ERROR: {path} not found.")
            if "video" in name:
                print("       Run: python tools\\prep_frames.py <input> [--fps 6]")
            else:
                print("       Run: build_flash_video.bat")
            sys.exit(1)

    with open(WRITER_BIN,  "rb") as f: writer_data  = f.read()
    with open(VIDEO_BIN,   "rb") as f: video_data   = f.read()
    with open(PLAYER_BIN,  "rb") as f: player_data  = f.read()

    n_pages   = (len(video_data) + PAGE_SIZE  - 1) // PAGE_SIZE
    n_sectors = (len(video_data) + SECTOR_SIZE - 1) // SECTOR_SIZE
    print(f"Writer firmware : {len(writer_data):,} B")
    print(f"Video blob      : {len(video_data):,} B  ({n_pages} pages, {n_sectors} sectors)")
    print(f"Player firmware : {len(player_data):,} B")

    lines = []
    lines.append("# combined_flash.tcl — generated by gen_flash.py")
    lines.append("# Phase 1: flash writer, Phase 2: write ext flash, Phase 3: flash player")
    lines.append("")

    # Phase 1 — flash writer firmware
    lines.append("# ============================================================")
    lines.append("# Phase 1: flash ext_flash_writer firmware to internal flash")
    lines.append("# ============================================================")
    lines += gen_internal_flash_tcl(writer_data, "ext_flash_writer")
    lines.append("reset run")
    lines.append("sleep 800")   # wait for MCU to boot + PLL to stabilise
    lines.append("")

    # Phase 2 — write video to external flash
    lines.append("# ============================================================")
    lines.append("# Phase 2: write video blob to external flash")
    lines.append("# ============================================================")
    lines += gen_ext_flash_write_tcl(video_data)

    # Phase 3 — flash player firmware
    lines.append("# ============================================================")
    lines.append("# Phase 3: flash flash_video player firmware")
    lines.append("# ============================================================")
    lines += gen_internal_flash_tcl(player_data, "flash_video")
    lines.append("reset run")
    lines.append('puts "All done — vape is now playing the video."')

    os.makedirs(os.path.dirname(OUT_TCL), exist_ok=True)
    with open(OUT_TCL, "w") as f:
        f.write("\n".join(lines) + "\n")

    print(f"\nGenerated: {OUT_TCL}  ({len(lines)} lines)")
    print("Next: flash_vape.bat")


if __name__ == "__main__":
    main()
