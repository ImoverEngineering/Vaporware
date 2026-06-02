# measure_fps.tcl — count frames by reading SRAM counter at 0x20000080
halt
sleep 30
catch {halt}
sleep 20

# Clear counter
mww 0x20000080 0

# Resume and wait 5 seconds
resume
sleep 5000

# Halt and read count
catch {halt}
sleep 30
catch {halt}

set cnt [mrw 0x20000080]
puts "Frames in 5 seconds: $cnt"
puts "FPS: [expr {$cnt / 5.0}]"

# Confirm clock state
puts "TIM3_PSC = [mrw 0x40000428]  (47=48MHz 7=8MHz)"
puts "SPI1_CR1 = 0x[format %08X [mrw 0x40012000]]"
puts "Sentinel: 0x[format %08X [mrw 0x20000040]]"

# CFGR snapshot captured in clock_boost_48mhz() — bits[13:4]=prescalers
# Expected: 0x20104002 (bit14=SWS_PLL, bits[21:18]=4=PLLMULL6, SW=10)
# If HPRE/PPRE1/PPRE2 bits non-zero → APB/AHB bus is divided!
set cfgr [mrw 0x20000050]
puts "CFGR snapshot: 0x[format %08X $cfgr]"
puts "  HPRE  bits\[7:4\]  = [expr {($cfgr >> 4)  & 0xF}]  (0=no div)"
puts "  PPRE1 bits\[10:8\] = [expr {($cfgr >> 8)  & 0x7}]  (0=no div)"
puts "  PPRE2 bits\[13:11\]= [expr {($cfgr >> 11) & 0x7}]  (0=no div)"

# Live RCC to double-check
set live_cfgr [mrw 0x40021004]
puts "RCC_CFGR live:  0x[format %08X $live_cfgr]"
puts "  HPRE  bits\[7:4\]  = [expr {($live_cfgr >> 4)  & 0xF}]"
puts "  PPRE1 bits\[10:8\] = [expr {($live_cfgr >> 8)  & 0x7}]"
puts "  PPRE2 bits\[13:11\]= [expr {($live_cfgr >> 11) & 0x7}]"

resume
exit
