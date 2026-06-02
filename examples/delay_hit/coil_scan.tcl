# coil_scan.tcl — scan candidate GPIOs to find the coil MOSFET gate.
# Drives each pin HIGH for 2 s, then LOW for 1 s, restoring it before moving on.
# User listens for the sizzle and notes which pin's "Testing ..." print matched.

init
adapter speed 1000
halt

# Enable GPIOA / GPIOB / GPIOC clocks
set apb2 [mrw 0x40021018]
mww 0x40021018 [expr {$apb2 | (1<<2) | (1<<3) | (1<<4)}]   ;# IOPAEN | IOPBEN | IOPCEN
sleep 5

# Test one pin: configure as push-pull output, drive HIGH 2 s, drive LOW, restore.
proc test_pin {label base pin} {
    set moder_addr [expr {$base + 0x00}]
    set odr_addr   [expr {$base + 0x14}]
    set bsrr_addr  [expr {$base + 0x18}]

    set m_orig [mrw $moder_addr]
    set m_new  [expr {($m_orig & ~(3 << ($pin*2))) | (1 << ($pin*2))}]

    puts "============================================="
    puts "Testing $label  (HIGH 2 s — listen)"
    puts "============================================="
    mww $moder_addr $m_new
    mww $bsrr_addr [expr {1 << $pin}]              ;# set HIGH
    sleep 2000
    mww $bsrr_addr [expr {1 << ($pin + 16)}]       ;# set LOW
    mww $moder_addr $m_orig                        ;# restore mode
    puts "  $label LOW, restored"
    sleep 1000
}

set GPIOA 0x40010800
set GPIOB 0x40010C00
set GPIOC 0x40011000

# GPIOA candidates
test_pin "PA0"  $GPIOA  0
test_pin "PA1"  $GPIOA  1
test_pin "PA2"  $GPIOA  2
test_pin "PA3"  $GPIOA  3
test_pin "PA8"  $GPIOA  8
test_pin "PA9"  $GPIOA  9
test_pin "PA10" $GPIOA 10
test_pin "PA11" $GPIOA 11
test_pin "PA12" $GPIOA 12

# GPIOB candidate
test_pin "PB1"  $GPIOB  1

# GPIOC candidates
test_pin "PC14" $GPIOC 14
test_pin "PC15" $GPIOC 15

puts ""
puts "Scan complete — which pin's 'Testing' line lined up with the sizzle?"
exit
