# adc_scan.tcl — sample ADC channels for battery-sense identification.

init
adapter speed 240
halt

mww 0x40021014 [expr {[mrw 0x40021014] | (1 << 12)}]
mww 0x40021018 [expr {[mrw 0x40021018] | (1 << 2) | (1 << 3)}]
mww 0x4002102C 0x00003804

# Set PA0..PA7 to analog (also lifts the SDK's PA4/5/6 LOW drives so we can
# observe what the external circuit actually puts on those pins).
set m [mrw 0x40010800]
for {set p 0} {$p <= 7} {incr p} { set m [expr {$m | (3 << ($p*2))}] }
mww 0x40010800 $m

# PB0, PB1 analog
set m [mrw 0x40010C00]
mww 0x40010C00 [expr {$m | 3 | (3<<2)}]

mww 0x40020810 0xFFFFFFFF
mww 0x4002080C 0xFFFFFFFF
mww 0x40020830 0x00000000

proc read_ch {ch label} {
    mww 0x40020838 $ch
    mww 0x40020808 0x00000001
    sleep 5
    set c [mrw 0x40020808]
    mww 0x40020808 [expr {$c | (7 << 17) | (1 << 20)}]
    mww 0x40020800 0x00000000
    set c [mrw 0x40020808]
    mww 0x40020808 [expr {$c | (1 << 22)}]
    set done 0
    for {set i 0} {$i < 500} {incr i} {
        if {[mrw 0x40020800] & 2} { set done 1; break }
    }
    if {$done == 0} {
        echo [format "  %-8s ch%2d  raw=TIMEOUT" $label $ch]
    } else {
        set raw [expr {[mrw 0x40020850] & 0xFFF}]
        set mv [expr {$raw * 3000 / 4096}]
        echo [format "  %-8s ch%2d  raw=%4d  pin_mV=%4d" $label $ch $raw $mv]
    }
}

read_ch 0 PA0
read_ch 1 PA1
read_ch 2 PA2
read_ch 3 PA3
read_ch 4 PA4
read_ch 5 PA5
read_ch 6 PA6
read_ch 7 PA7
read_ch 8 PB0
read_ch 9 PB1

shutdown
