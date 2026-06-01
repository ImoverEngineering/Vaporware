# sentinels.tcl — read clock_boost sentinels and key registers
halt
sleep 50
catch {halt}
sleep 20

puts "=== clock_boost_48mhz sentinels ==="
puts "0x20000040: 0x[format %08X [mrw 0x20000040]]  (AA001111 = entered)"
puts "0x20000044: 0x[format %08X [mrw 0x20000044]]  (BB002222 = PLLON set)"
puts "0x20000048: 0x[format %08X [mrw 0x20000048]]  (CC003333=ok  EE005555=FAILED)"
puts "0x2000004c: 0x[format %08X [mrw 0x2000004c]]  (DD004444 = full success)"
puts "0x20000050: 0x[format %08X [mrw 0x20000050]]  (CFGR snapshot, bit14=SWS_PLL)"
puts ""
puts "=== RCC live (base=0x40021000) ==="
puts "RCC_CR   (0x40021000): 0x[format %08X [mrw 0x40021000]]  (bit24=PLLON bit25=PLLRDY)"
puts "RCC_CFGR (0x40021004): 0x[format %08X [mrw 0x40021004]]  (bits1:0=SW bits3:2=SWS)"
puts ""
puts "=== SPI + TIM3 ==="
puts "SPI1_CR1 (0x40012000): 0x[format %08X [mrw 0x40012000]]  BR=[expr {([mrw 0x40012000]>>3)&7}]"
puts "TIM3_PSC (0x40000428): [mrw 0x40000428]  (47=48MHz 7=8MHz)"
puts "PC when halted: [mrw 0xE000101C]"

resume
exit
