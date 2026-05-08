#!/usr/bin/env python3
"""battery_check.py — Read battery voltage from N32G031 via SWD/ADC.

Uses the same OpenOCD server-mode + telnet approach as stream_frames.py.
Halts the MCU, triggers an ADC conversion on CH8 (PB0 = VBAT divider),
reads the result, converts to voltage, then resumes the MCU.

Thresholds (from battery.c):
    BAT_FULL  >= 181  (~3.70 V, battery full)
    BAT_WARN  >= 146  (~3.00 V, warn user)
    BAT_CRIT  >= 122  (~2.50 V, cut off power)
"""

import re
import socket
import subprocess
import sys
import time

# ── OpenOCD / WSL config ──────────────────────────────────────────────────────
# Use the local custom cfg — it has no stm32f1x flash bank and no bad examine-end
# hook (which writes to 0x40007030, an invalid address on N32G031 that makes
# OpenOCD exit rc=1 before port 4444 ever opens).
# We pass -c "init" -c "reset halt" on the command line so the server loop
# starts cleanly after the halt.
OCD_TARGET_CFG = (
    "/mnt/c/Users/cooli/Claude_Vapes/Vaporware/examples/streamer/"
    "n32g031.openocd.cfg"
)
USBIPD_BUSID   = "1-2"
OCD_TELNET_PORT = 4444

# ── ADC registers (N32G031, from battery.c) ───────────────────────────────────
RCC_AHBENR   = 0x40021014   # bit 12 = ADC1 clock enable
RCC_CFGR2    = 0x4002102C   # ADC prescaler  (write 0x00003804)
RCC_APB2ENR  = 0x40021018   # bit 3  = GPIOB clock enable

GPIOB_MODER  = 0x40010C00   # PB0 bits[1:0]=11 → analog

ADC_STS      = 0x40020800   # status (clear EOC here)
ADC_CTRL2    = 0x40020808   # ADON bit0, EXTSEL[20:17]=7 sw, EXTTRIG bit20, SWSTRRCH bit22
ADC_SAMPT2   = 0x40020810   # CH8 sample time bits[26:24]=111 (239.5 cycles)
ADC_RSEQ1    = 0x40020830   # regular sequence length (0 = 1 conversion)
ADC_RSEQ3    = 0x40020838   # first conversion channel
ADC_DAT      = 0x40020850   # result (bits[11:0])

# CTRL2 bit fields
CTRL2_ADON      = 0x00000001
CTRL2_EXTSEL    = 0x000E0000   # EXTSEL[3:0]=0111 (software trigger)
CTRL2_EXTTRIG   = 0x00100000
CTRL2_SWSTRRCH  = 0x00400000

# Thresholds (raw 12-bit ADC, from battery.c)
BAT_FULL = 181
BAT_WARN = 146
BAT_CRIT = 122

_ANSI_RE = re.compile(rb'\x1b\[[^m]*m')


def raw_to_voltage(raw):
    """Convert 12-bit ADC raw → battery voltage.
    From battery.c: V_bat = raw * 1.41 * 3.0 / 4096
    (empirical divider ratio ~1:28, VDDA=3.0V)
    """
    return raw * 1.41 * 3.0 / 4096.0


class OcdSession:
    """Minimal OpenOCD telnet session (same logic as VapeDisplay in stream_frames.py)."""

    def __init__(self):
        self._sock = None
        self._ocd_proc = None
        self._wsl_keepalive = None
        self._rxbuf = b''

    def connect(self):
        print("[1/5] Waking WSL…")
        subprocess.run(['wsl', 'echo', 'ready'], capture_output=True, timeout=10)

        print("[2/5] Starting WSL keepalive…")
        self._wsl_keepalive = subprocess.Popen(
            ['wsl', '-u', 'root', 'bash', '-c', 'sleep infinity'],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(0.5)

        print("[3/5] Attaching ST-Link to WSL…")
        r = subprocess.run(
            ['usbipd', 'attach', '--wsl', '--busid', USBIPD_BUSID],
            capture_output=True, text=True
        )
        if r.returncode != 0:
            print(f"  WARNING: usbipd returned {r.returncode}: {r.stderr.strip()}")
        time.sleep(1.5)

        print("[4/5] Starting OpenOCD (server mode)…")
        # Custom cfg has no stm32f1x flash bank and no bad examine-end hook.
        # -c "init" and -c "reset halt" are explicit so the server loop starts
        # cleanly (the cfg itself does NOT call init/reset halt at the end).
        ocd_cmd = (
            f'openocd -f {OCD_TARGET_CFG} '
            f'-c "init" '
            f'-c "reset halt"'
        )
        self._ocd_proc = subprocess.Popen(
            ['wsl', '-u', 'root', 'bash', '-c', ocd_cmd],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(5.0)   # wait for OpenOCD to connect + reset-halt MCU

        rc = self._ocd_proc.poll()
        if rc is not None:
            raise RuntimeError(f"OpenOCD exited early (rc={rc})")

        print("[5/5] Connecting telnet to OpenOCD…")
        self._sock = socket.create_connection(('localhost', OCD_TELNET_PORT), timeout=5)
        self._sock.settimeout(15)
        self._read_prompt()   # consume banner + first prompt
        print("      Connected.\n")

    def _read_prompt(self, timeout=8.0):
        deadline = time.monotonic() + timeout
        self._sock.settimeout(0.05)
        try:
            while True:
                stripped = _ANSI_RE.sub(b'', self._rxbuf)
                idx = stripped.find(b'>')
                if idx >= 0:
                    response = stripped[:idx + 1].decode('utf-8', errors='replace')
                    self._rxbuf = stripped[idx + 1:]
                    return response
                if time.monotonic() >= deadline:
                    break
                try:
                    data = self._sock.recv(4096)
                    if not data:
                        break
                    self._rxbuf += data
                except socket.timeout:
                    pass
        finally:
            self._sock.settimeout(15)
        result = _ANSI_RE.sub(b'', self._rxbuf).decode('utf-8', errors='replace')
        self._rxbuf = b''
        return result

    def cmd(self, command, timeout=8.0):
        self._sock.send((command + '\n').encode())
        return self._read_prompt(timeout=timeout)

    def mrw(self, addr):
        """Read a 32-bit word; return as integer.

        OpenOCD telnet echoes the command before printing the result, so the
        response looks like:  "mrw 0xADDR\\r\\n0xVALUE\\r\\n> "
        Taking the LAST hex match skips the address in the echo and picks
        up the actual register value.
        """
        resp = self.cmd(f'mrw 0x{addr:08X}')
        matches = re.findall(r'0x([0-9a-fA-F]+)', resp)
        return int(matches[-1], 16) if matches else 0

    def mww(self, addr, val):
        """Write a 32-bit word."""
        self.cmd(f'mww 0x{addr:08X} 0x{val:08X}')

    def rmw_or(self, addr, or_val):
        """Read-modify-write: set bits in or_val."""
        cur = self.mrw(addr)
        self.mww(addr, cur | or_val)

    def close(self, resume=True):
        if resume:
            try:
                self.cmd('resume')
                print("  MCU resumed.")
            except Exception:
                pass
        try:
            self.cmd('shutdown')
        except Exception:
            pass
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
        if self._ocd_proc:
            try:
                self._ocd_proc.wait(timeout=3)
            except Exception:
                self._ocd_proc.terminate()
        if self._wsl_keepalive:
            try:
                self._wsl_keepalive.terminate()
            except Exception:
                pass


def read_battery(ocd):
    """Trigger ADC CH8 (PB0) conversion and return (raw, voltage)."""

    print("  Halting MCU…")
    ocd.cmd('halt')
    time.sleep(0.1)

    print("  Configuring ADC…")

    # 1. Enable ADC1 clock (AHBENR bit 12)
    ocd.rmw_or(RCC_AHBENR, 0x1000)
    ahbenr = ocd.mrw(RCC_AHBENR)
    print(f"    AHBENR  = 0x{ahbenr:08X}  (bit12 ADC1 = {'SET' if ahbenr & 0x1000 else 'CLEAR'})")

    # 2. ADC prescaler — must be set before ADON
    ocd.mww(RCC_CFGR2, 0x00003804)
    cfgr2 = ocd.mrw(RCC_CFGR2)
    print(f"    CFGR2   = 0x{cfgr2:08X}  (expect 0x00003804)")

    # 3. Enable GPIOB clock (APB2ENR bit 3)
    ocd.rmw_or(RCC_APB2ENR, 0x0008)
    apb2 = ocd.mrw(RCC_APB2ENR)
    print(f"    APB2ENR = 0x{apb2:08X}  (bit3 GPIOB = {'SET' if apb2 & 0x8 else 'CLEAR'})")

    # 4. PB0 → analog mode (MODER bits[1:0] = 11)
    ocd.rmw_or(GPIOB_MODER, 0x0003)
    moder = ocd.mrw(GPIOB_MODER)
    print(f"    MODER   = 0x{moder:08X}  (PB0 bits[1:0] = {moder & 0x3:02b} expect 11)")

    # 5. ADC channel 8 sample time = 239.5 cycles (bits[26:24] = 111)
    ocd.rmw_or(ADC_SAMPT2, 0x07000000)
    smpr2 = ocd.mrw(ADC_SAMPT2)
    print(f"    SMPR2   = 0x{smpr2:08X}  (ch8 bits[26:24] = {(smpr2>>24)&7} expect 7)")

    # 6. Regular sequence: 1 conversion (RSEQ1 L[3:0]=0), channel 8 in RSEQ3
    ocd.mww(ADC_RSEQ1, 0x00000000)
    ocd.mww(ADC_RSEQ3, 0x00000008)
    rseq3 = ocd.mrw(ADC_RSEQ3)
    print(f"    RSEQ3   = 0x{rseq3:08X}  (expect 0x00000008)")

    # 7. Power on ADC — ADON (bit0) alone first, like battery.c
    ocd.mww(ADC_CTRL2, CTRL2_ADON)
    time.sleep(0.002)   # 2 ms stabilisation (ADC power-up)

    # 8. Then set EXTSEL[3:1]=7 (software trigger) + EXTTRIG (bit20)
    ocd.rmw_or(ADC_CTRL2, CTRL2_EXTSEL | CTRL2_EXTTRIG)
    ctrl2 = ocd.mrw(ADC_CTRL2)
    print(f"    CTRL2   = 0x{ctrl2:08X}  (expect ~0x001E0001)")

    # 9. Clear status register
    ocd.mww(ADC_STS, 0x00000000)

    # 10. Start conversion (SWSTRRCH = bit22)
    ocd.rmw_or(ADC_CTRL2, CTRL2_SWSTRRCH)
    ctrl2_after = ocd.mrw(ADC_CTRL2)
    print(f"    CTRL2 after SWSTRRCH = 0x{ctrl2_after:08X}  (bit22 = {'SET' if ctrl2_after & 0x400000 else 'CLEAR'})")

    # 11. Wait for EOC (bit 1 of ADC_STS); poll up to 200 ms
    print("  Waiting for EOC…")
    raw = 0
    eoc_found = False
    for i in range(200):
        time.sleep(0.001)
        sts = ocd.mrw(ADC_STS)
        if sts & 0x2:   # EOC set
            raw = ocd.mrw(ADC_DAT) & 0xFFF
            print(f"    EOC after {i+1} ms  STS=0x{sts:08X}  DAT=0x{raw:03X} ({raw})")
            eoc_found = True
            break

    if not eoc_found:
        sts = ocd.mrw(ADC_STS)
        raw_dat = ocd.mrw(ADC_DAT)
        print(f"  WARNING: EOC never set after 200 ms")
        print(f"    Final STS  = 0x{sts:08X}")
        print(f"    Final DAT  = 0x{raw_dat:08X}  ({raw_dat & 0xFFF})")
        raw = raw_dat & 0xFFF

    return raw, raw_to_voltage(raw)


def read_vrefint(ocd):
    """Read internal VREF (CH17) as a sanity check.
    N32G031: CTRL2 bit23=TSVREFE enables temp+vref channels.
    Expected VREFINT ≈ 1.2 V → raw ≈ 1638 with VDDA=3.0 V.
    """
    # Enable VREFINT: CTRL2 bit 23 = TSVREFE
    ocd.rmw_or(ADC_CTRL2, 1 << 23)

    # Channel 17 sample time bits[27:24]? No — SMPR1 handles ch10-17.
    # SMPR1 is at ADC_BASE + 0x0C = 0x4002080C
    # CH17 = bits[23:21] in SMPR1. Set to 111 (239.5 cycles).
    ADC_SMPR1 = 0x4002080C
    ocd.rmw_or(ADC_SMPR1, 0x00E00000)   # ch17 bits[23:21]=111

    # Convert channel 17
    ocd.mww(ADC_RSEQ3, 17)
    ocd.mww(ADC_STS, 0)
    ocd.rmw_or(ADC_CTRL2, CTRL2_SWSTRRCH)

    vref_raw = 0
    for i in range(200):
        time.sleep(0.001)
        if ocd.mrw(ADC_STS) & 0x2:
            vref_raw = ocd.mrw(ADC_DAT) & 0xFFF
            break

    # Restore CH8 in RSEQ3 for any follow-up reads
    ocd.mww(ADC_RSEQ3, 8)
    return vref_raw


def main():
    ocd = OcdSession()
    try:
        ocd.connect()
        raw, volts = read_battery(ocd)

        # Sanity check: read internal VREF (should be ~1.2V → raw≈1638 at VDDA=3.0V)
        vref_raw = read_vrefint(ocd)
        vref_volts = vref_raw * 3.0 / 4096.0
        print(f"\n  VREFINT sanity check: raw={vref_raw}  ({vref_volts:.3f} V, expect ~1.2 V)")
        if vref_raw < 500 or vref_raw > 3000:
            print("  *** VREFINT out of expected range — ADC config may be wrong ***")
        else:
            print("  VREFINT OK — ADC is working correctly")

        print(f"\n{'='*40}")
        print(f"  Battery ADC raw : {raw}")
        print(f"  Battery voltage : {volts:.2f} V  (approx)")
        if raw >= BAT_FULL:
            status = "FULL / CHARGING"
        elif raw >= BAT_WARN:
            status = "OK"
        elif raw >= BAT_CRIT:
            status = "LOW — warn user"
        else:
            status = "CRITICAL — may be too low to power display!"
        print(f"  Status          : {status}")
        print(f"{'='*40}\n")

        if raw < BAT_CRIT:
            print("  *** Battery may be too discharged to power the GC9107 display. ***")
            print("  *** Charge before further testing.                              ***")

    finally:
        ocd.close(resume=True)


if __name__ == '__main__':
    main()
