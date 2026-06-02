# coil_test.tcl — drive PB0 HIGH for 3 seconds via SWD, no firmware involved.
# Tests whether PB0 → coil-MOSFET → coil hardware path actually works on this device.

init
adapter speed 1000
halt

# Enable GPIOB clock (RCC APB2ENR bit 3)
set apb2 [mrw 0x40021018]
mww 0x40021018 [expr {$apb2 | (1 << 3)}]

# Read current GPIOB MODER
set m [mrw 0x40010C00]
puts "MODER before: 0x[format %08X $m]"

# Set PB0 (bits 0-1) to 01 = push-pull output, leave other pins alone
set new_m [expr {($m & ~3) | 1}]
mww 0x40010C00 $new_m
puts "MODER after : 0x[format %08X $new_m]"

# BSRR bit 0 = set PB0 HIGH
mww 0x40010C18 0x00000001
puts "PB0 driven HIGH — listen for ~3 s"
puts "ODR = 0x[format %08X [mrw 0x40010C14]]"

sleep 3000

# BSRR bit 16 = clear PB0 (drive LOW)
mww 0x40010C18 0x00010000
puts "PB0 driven LOW"
puts "ODR = 0x[format %08X [mrw 0x40010C14]]"

# Restore PB0 to input
set m [mrw 0x40010C00]
mww 0x40010C00 [expr {$m & ~3}]
puts "PB0 returned to input"

exit
