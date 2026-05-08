# halt_vape.tcl — Turn off vape display and halt MCU via OpenOCD
#
# n32g0x.cfg already called init + reset halt, so the firmware is frozen
# at its very first instruction.  We just resume it, let it run through
# display init, send CTRL_SLEEP, then halt.
#
# Usage (from the streamer directory, WSL):
#   openocd -f interface/stlink.cfg \
#           -c "adapter speed 1000" \
#           -f /mnt/c/.../openocd/scripts/target/n32g0x.cfg \
#           -f halt_vape.tcl

echo "halt_vape: resuming firmware..."

# Let firmware run — display_init() takes ~300 ms at 8 MHz
resume
sleep 700

echo "halt_vape: writing CTRL_SLEEP..."
# CTRL_SLEEP = 0xDEAD0001 → firmware calls display_sleep_in() then
# enters an IWDG-feeding loop
mww 0x20000010 0xDEAD0001

# display_sleep_in(): backlight off + Display Off + Sleep In ≈ 60 ms
sleep 300

echo "halt_vape: halting MCU..."
halt

set pc_val [capture {reg pc}]
echo "Halted. $pc_val"
echo "LCD is off. Done."
shutdown
