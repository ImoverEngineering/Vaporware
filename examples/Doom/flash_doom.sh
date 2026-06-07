#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if [[ ! -f build/doom.bin ]]; then
    echo "ERROR: build/doom.bin not found. Run ./build_doom.sh first." >&2
    exit 1
fi

if [[ ! -f direct_flash.tcl ]]; then
    echo "ERROR: direct_flash.tcl not found. Run python3 gen_direct_flash.py first." >&2
    exit 1
fi

openocd \
    -f n32g031.openocd.cfg \
    -c "tcl_port disabled; telnet_port disabled; gdb_port disabled" \
    -c "source direct_flash.tcl" \
    -c "exit"
