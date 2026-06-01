#!/usr/bin/env bash
# examples/doom/build.sh - Linux build script for the Vaporware Doom mini-game
set -euo pipefail

APP_NAME="doom"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VAPORWARE="${SCRIPT_DIR}/../../src"
BUILD_DIR="${SCRIPT_DIR}/build"

# Override these if your toolchain uses different names/paths:
#   CC=/path/to/arm-none-eabi-gcc OBJCOPY=/path/to/arm-none-eabi-objcopy SIZE=/path/to/arm-none-eabi-size ./build.sh
CC="${CC:-arm-none-eabi-gcc}"
OBJCOPY="${OBJCOPY:-arm-none-eabi-objcopy}"
SIZE="${SIZE:-arm-none-eabi-size}"

CPU=(-mcpu=cortex-m0 -mthumb)
INC=(-I"${VAPORWARE}/include" -I"${SCRIPT_DIR}/include")
CFLAGS=("${CPU[@]}" "${INC[@]}" -Os -ffunction-sections -fdata-sections -Wall -std=c11)
LDFLAGS=("${CPU[@]}" -T"${VAPORWARE}/n32g031.ld" -Wl,--gc-sections -Wl,-Map="${BUILD_DIR}/${APP_NAME}.map" -nostdlib -lnosys)

mkdir -p "${BUILD_DIR}"

echo "[1/12] startup.s (vaporware)"
"${CC}" "${CPU[@]}" -x assembler-with-cpp -c "${VAPORWARE}/src/startup.s" -o "${BUILD_DIR}/startup.o"

echo "[2/12] system.c (vaporware)"
"${CC}" "${CFLAGS[@]}" -c "${VAPORWARE}/src/system.c" -o "${BUILD_DIR}/system.o"

echo "[3/12] display.c (vaporware)"
"${CC}" "${CFLAGS[@]}" -c "${VAPORWARE}/src/display.c" -o "${BUILD_DIR}/display.o"

echo "[4/12] vape.c (vaporware)"
"${CC}" "${CFLAGS[@]}" -c "${VAPORWARE}/src/vape.c" -o "${BUILD_DIR}/vape.o"

echo "[5/12] button.c (vaporware)"
"${CC}" "${CFLAGS[@]}" -c "${VAPORWARE}/src/button.c" -o "${BUILD_DIR}/button.o"

echo "[6/12] battery.c (vaporware)"
"${CC}" "${CFLAGS[@]}" -c "${VAPORWARE}/src/battery.c" -o "${BUILD_DIR}/battery.o"

echo "[7/12] nv.c (vaporware)"
"${CC}" "${CFLAGS[@]}" -c "${VAPORWARE}/src/nv.c" -o "${BUILD_DIR}/nv.o"

echo "[8/12] app.c (vaporware)"
"${CC}" "${CFLAGS[@]}" -c "${VAPORWARE}/src/app.c" -o "${BUILD_DIR}/app.o"

echo "[9/12] doom_title_letterbox.c (title art)"
"${CC}" "${CFLAGS[@]}" -c "${SCRIPT_DIR}/src/doom_title_letterbox.c" -o "${BUILD_DIR}/doom_title_letterbox.o"

echo "[10/12] doom_enemy_sprites.c (enemy sheet)"
"${CC}" "${CFLAGS[@]}" -c "${SCRIPT_DIR}/src/doom_enemy_sprites.c" -o "${BUILD_DIR}/doom_enemy_sprites.o"

echo "[11/12] doom_deathscreen.c (death screen)"
"${CC}" "${CFLAGS[@]}" -c "${SCRIPT_DIR}/src/doom_deathscreen.c" -o "${BUILD_DIR}/doom_deathscreen.o"

echo "[12/12] main.c (doom app)"
"${CC}" "${CFLAGS[@]}" -c "${SCRIPT_DIR}/src/main.c" -o "${BUILD_DIR}/main.o"

echo "Linking..."
"${CC}" "${LDFLAGS[@]}" \
  "${BUILD_DIR}/startup.o" "${BUILD_DIR}/system.o" "${BUILD_DIR}/display.o" "${BUILD_DIR}/vape.o" \
  "${BUILD_DIR}/button.o" "${BUILD_DIR}/battery.o" "${BUILD_DIR}/nv.o" "${BUILD_DIR}/app.o" \
  "${BUILD_DIR}/doom_title_letterbox.o" "${BUILD_DIR}/doom_enemy_sprites.o" "${BUILD_DIR}/doom_deathscreen.o" \
  "${BUILD_DIR}/main.o" \
  -o "${BUILD_DIR}/${APP_NAME}.elf"

"${OBJCOPY}" -O binary "${BUILD_DIR}/${APP_NAME}.elf" "${BUILD_DIR}/${APP_NAME}.bin"
"${OBJCOPY}" -O ihex "${BUILD_DIR}/${APP_NAME}.elf" "${BUILD_DIR}/${APP_NAME}.hex"
"${SIZE}" "${BUILD_DIR}/${APP_NAME}.elf"

echo
echo "Build SUCCESS: ${BUILD_DIR}/${APP_NAME}.bin"
