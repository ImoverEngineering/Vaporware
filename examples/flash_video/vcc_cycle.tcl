# vcc_cycle.tcl — VCC cut then full MCU reset so firmware re-inits the display

catch {halt}
sleep 100
catch {halt}
sleep 50

# Enable GPIOA clock
set apb2enr [mrw 0x40021018]
mww 0x40021018 [expr {$apb2enr | 0x4}]

# Set PA4, PA5, PA6 as outputs
set moder [mrw 0x40010800]
set moder [expr {$moder & ~0x00003F00}]
set moder [expr {$moder | 0x00001500}]
mww 0x40010800 $moder
set otyper [mrw 0x40010804]
mww 0x40010804 [expr {$otyper & ~0x70}]

puts "CUT: PA4/5/6 HIGH — display VCC off"
mww 0x40010818 [expr {(1<<4) | (1<<5) | (1<<6)}]
puts "ODR = 0x[format %08X [mrw 0x40010814]]"

sleep 1000

puts "RESTORE: PA4/5/6 LOW — display VCC on"
mww 0x40010818 [expr {(1<<20) | (1<<21) | (1<<22)}]
puts "ODR = 0x[format %08X [mrw 0x40010814]]"

sleep 200

puts "Reset MCU — fw_dump.bin boots and re-inits display..."
reset run
sleep 2000
exit
