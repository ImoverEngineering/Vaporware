# Doom-ish for Vaporware

A tiny colored Doom-style mini game for the Vaporware SDK / Raz DC25000 N32G031 vape target.

It is not a real Doom engine and it does not load WAD files. It is a small, self-contained, colored fake-3D shooter designed to fit the N32G031K8Q7-1 constraints.

## Controls

- Title screen: press the button to start.
- Game: auto-walks forward through the corridor.
- Tap button: shoot.
- Hold button: strafe/turn effect while held.
- Lose/win screen: press button to restart.

## Build

Copy `examples/doom` into the Vaporware repo, then from Windows Command Prompt:

```bat
cd examples\doom
build_doom.bat
```

Output:

```text
examples\doom\build\doom.bin
```

Then flash the same way you flashed Flappy.

## Assets

- Enemy sprites are generated from `src/enemys sheet.png`.
- The death-screen reference comes from `src/Deathscreen.jpg`, but the in-game death screen is composed to match the Doom HUD/style instead of showing the image full-screen.
- To regenerate the asset C files after changing either image, run `python3 tools/gen_doom_assets.py` from `examples/Doom`.
