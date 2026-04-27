#!/bin/bash
# build_sim.sh — compile slot machine sim with SDL2 under WSL
set -e

SLOTS=/mnt/c/Users/cooli/Claude_Vapes/Vaporware/slotmachine
VAPOR=/mnt/c/Users/cooli/Claude_Vapes/Vaporware/vaporware
OUT="$SLOTS/sim/slots_sim"

echo "[Building slot machine sim...]"
gcc -O2 -Wall -Wextra \
    -I"$VAPOR/include" \
    -I"$SLOTS/include" \
    -I"$SLOTS/sim" \
    "$SLOTS/src/slots.c" \
    "$SLOTS/sim/display_stub.c" \
    "$SLOTS/sim/system_stub.c" \
    "$SLOTS/sim/vape_stub.c" \
    "$SLOTS/sim/sim_main.c" \
    $(sdl2-config --cflags --libs) \
    -lm \
    -o "$OUT"

echo "[Build OK → $OUT]"
echo "Running..."
"$OUT"
