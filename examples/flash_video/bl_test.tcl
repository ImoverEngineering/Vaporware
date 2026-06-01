catch {halt}
sleep 100
catch {halt}
sleep 30

# Enable GPIOB clock
set apb2 [mrw 0x40021018]
mww 0x40021018 [expr {$apb2 | 0x8}]

# Ensure PB4 is output (set MODER bits[9:8] = 01)
set moder [mrw 0x40010C00]
set moder [expr {($moder & ~0x300) | 0x100}]
mww 0x40010C00 $moder

puts "PB4=HIGH now (BL OFF if active-low)"
mww 0x40010C18 [expr {1 << 4}]
puts "ODR = 0x[format %08X [mrw 0x40010C14]]"
sleep 2000

puts "PB4=LOW now (BL ON if active-low)"
mww 0x40010C18 [expr {1 << 20}]
puts "ODR = 0x[format %08X [mrw 0x40010C14]]"
sleep 2000

puts "Toggle test done"
resume
