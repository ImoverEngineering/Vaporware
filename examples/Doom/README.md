# Doom-ish for Vaporware

`Doom-ish` is a tiny one-button, fake-3D shooter built for the Vaporware SDK and the Raz DC25000 / N32G031K8Q7-1 vape target.

It is not the real Doom engine, and it does not load WAD files. This example is a small self-contained game with Doom-inspired art, enemies, HUD, and pacing, sized to fit the device's 64 KB flash / 8 KB SRAM limits.

## What It Does

- Shows a custom Doom-style title screen
- Auto-moves the player through a corridor
- Spawns multiple enemy types plus a periodic boss encounter
- Tracks health, ammo, reload state, win, and death screens
- Uses prebuilt 4bpp art assets for the title, enemies, and death screen

## Controls

- Title screen: press the button to start
- In game: tap the button to shoot
- In game: hold the button to slow movement for a strafe/turn feel
- Win or death screen: press the button to return to the title screen
- Safety escape: hold the button for about 10 seconds to jump back to the title screen

## Objective

Survive the corridor, manage your 6-shot magazine, and keep pushing forward until you reach the exit. Every sixth kill queues up a boss enemy, and killing enemies advances your progress faster.

## Files

| File | Purpose |
|---|---|
| `src/main.c` | Main game logic, rendering, controls, and state machine |
| `src/doom_title_letterbox.c` | Title screen image asset and draw routine |
| `src/doom_enemy_sprites.c` | Enemy sprite sheet converted into indexed C data |
| `src/doom_deathscreen.c` | Doom-style death screen asset and draw routine |
| `tools/gen_doom_assets.py` | Regenerates C assets from the source images |
| `build_doom.bat` / `build_doom.sh` | Build scripts for Windows and Linux |
| `gen_direct_flash.py` | Converts `build/doom.bin` into `direct_flash.tcl` |
| `flash_vape.bat` / `flash_doom.sh` | OpenOCD flashing helpers |

## Build

This example lives under `Vaporware/examples/Doom` and builds against the shared SDK in `Vaporware/src`.

### Windows

From Command Prompt:

```cmd
cd Vaporware\examples\Doom
build_doom.bat
```

Expected output:

```text
build\doom.bin
build\doom.elf
build\doom.hex
build\doom.map
```

`build_doom.bat` assumes Arm GNU Toolchain 14.2 is installed at:

```text
C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\
```

If your toolchain is somewhere else, update the `GCC`, `OBJCOPY`, and `SIZE` variables at the top of the batch file.

### Linux / WSL

From a shell:

```bash
cd Vaporware/examples/Doom
./build_doom.sh
```

The script expects `arm-none-eabi-gcc`, `arm-none-eabi-objcopy`, and `arm-none-eabi-size` to be on `PATH`. You can also override them inline:

```bash
CC=/path/to/arm-none-eabi-gcc \
OBJCOPY=/path/to/arm-none-eabi-objcopy \
SIZE=/path/to/arm-none-eabi-size \
./build_doom.sh
```

## Flashing

### Windows helper flow

```cmd
cd Vaporware\examples\Doom
python gen_direct_flash.py
flash_vape.bat
```

`flash_vape.bat`:

- checks that `build\doom.bin` exists
- checks that `direct_flash.tcl` exists
- attaches the ST-Link into WSL with `usbipd`
- runs OpenOCD with `n32g031.openocd.cfg`

The batch file currently hardcodes `usbipd attach --wsl --busid 1-2`, so change that bus ID if your ST-Link shows up differently on your machine.

### Linux / WSL helper flow

```bash
cd Vaporware/examples/Doom
python3 gen_direct_flash.py
./flash_doom.sh
```

`flash_doom.sh` runs OpenOCD directly and expects `direct_flash.tcl` plus `build/doom.bin` to already exist.

### Tigard manual OpenOCD flow

If you are using a Tigard instead of an ST-Link, you can flash Doom manually with the Tigard SWD config in the repo root.

Build and generate the flash script first:

```bash
cd Vaporware/examples/Doom
bash build_doom.sh
python3 gen_direct_flash.py
```

Then start OpenOCD in a separate terminal:

```bash
sudo openocd \
  -f ../../../tigard-swd.cfg \
  -f n32g031.openocd.cfg
```

Open a telnet session to the OpenOCD server:

```bash
telnet localhost 4444
```

Run these OpenOCD commands inside the telnet session:

```text
init
reset halt
source direct_flash.tcl
reset run
shutdown
```

Notes:

- Use `n32g031.openocd.cfg` from `Vaporware/examples/Doom`; that is the matching target config present in this repo
- Do not disable `telnet_port` before connecting, or the `telnet localhost 4444` step will fail
- If you prefer a one-shot command instead of telnet, you can pass `-c "source ..."` and `-c "exit"` directly to `openocd`

## Asset Pipeline

The game uses source images in `src/` and checked-in generated C assets.

- `src/enemys sheet.png` is sliced into multiple indexed enemy sprites
- `src/Deathscreen.jpg` is converted into the stylized in-game death screen asset
- The title art is stored separately as `doom_title_letterbox.*`

After changing the enemy sheet or death-screen reference, regenerate the C files with:

```bash
cd Vaporware/examples/Doom
python3 tools/gen_doom_assets.py
```

This updates:

- `src/doom_enemy_sprites.h`
- `src/doom_enemy_sprites.c`
- `src/doom_deathscreen.h`
- `src/doom_deathscreen.c`

## Notes

- The game is intentionally simple and uses broad shapes plus low-frame-rate animation to fit the hardware
- Rendering targets the 128x160 GC9107 display used by the Vaporware platform
- This example uses the Vaporware app framework, button handling, display driver, battery support, and startup code from `../../src`
