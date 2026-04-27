# Dumping the Raz DC25000 Firmware

The original factory firmware is not included in this repo (it is proprietary to the manufacturer).
These instructions let you dump your own device's flash before overwriting it, so you can restore
it if needed.

---

## What You Need

- ST-Link V2 (~$8 on Amazon)
- WSL 2 + [usbipd-win](https://github.com/dorssel/usbipd-win)
- OpenOCD (installed inside WSL: `sudo apt install openocd`)
- The OpenOCD config from any example: `examples/flappy/n32g031.openocd.cfg`

---

## 1 — Wire the ST-Link

| ST-Link pin | Vape test pad |
|---|---|
| SWDIO | PA13 |
| SWCLK | PA14 |
| GND | GND |
| 3.3 V | **leave disconnected** (vape is self-powered) |

---

## 2 — Attach ST-Link to WSL

In a Windows Command Prompt or PowerShell (run as Administrator):

```cmd
usbipd list
usbipd attach --wsl --busid <busid>
```

Substitute the bus ID shown for your ST-Link (typically `1-2` or similar).

---

## 3 — Start OpenOCD

In a WSL terminal, from the repo root:

```bash
openocd -f examples/flappy/n32g031.openocd.cfg
```

Leave this terminal open. OpenOCD will print `Info : Listening on port 3333` and
`Info : Listening on port 6666` when ready.

The config uses `CPUTAPID 0` to skip the IDCODE check — the N32G031 returns `0x0bb11477`,
which OpenOCD's stm32g0x driver rejects without this override.

---

## 4 — Dump the Flash

In a second WSL terminal:

```bash
# Full 64 KB flash dump
telnet localhost 4444
```

At the OpenOCD prompt:

```
halt
dump_image /mnt/c/Users/<you>/fw_dump.bin 0x08000000 0x10000
resume
exit
```

This dumps all 64 KB (0x10000 bytes) starting at the flash base address.
The output file is a raw binary image.

Alternatively, do it in one line without the interactive telnet session:

```bash
openocd -f examples/flappy/n32g031.openocd.cfg \
  -c "init; halt; dump_image /mnt/c/Users/<you>/fw_dump.bin 0x08000000 0x10000; resume; shutdown"
```

---

## 5 — Restore the Factory Firmware

Flash the dump back the same way you flash any other binary.
Edit `tools/flash_charge.py` and set:

```python
BIN_PATH = "/mnt/c/Users/<you>/fw_dump.bin"
```

Then run `flash_charge.py` with an OpenOCD telnet session already running on port 6666.

---

## Notes

- The N32G031K8Q7-1 has **64 KB flash** at `0x08000000–0x0800FFFF`.
- The top 4 KB (`0x0800F000–0x0800FFFF`) is reserved by the Vaporware NV storage module.
  The factory firmware does not use this region, so a raw 64 KB dump round-trips cleanly.
- SWD does not require the device to be powered by the ST-Link. The 500 mAh LiPo provides
  power; just leave the ST-Link 3.3 V pin disconnected.
