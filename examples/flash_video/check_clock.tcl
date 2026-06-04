init
halt
sleep 50
halt

# SPI1 base = 0x40012000 (APB2 + 0x2000)
set spi_cr1 [mrw 0x40012000]
set spi_sr  [mrw 0x40012008]
puts "SPI1_CR1 = 0x[format %08X $spi_cr1]"
puts "  BR   bits\[5:3\] = [expr {($spi_cr1 >> 3) & 7}]  (3=DIV16=3MHz  0=DIV2=24MHz)"
puts "  SPE  bit6      = [expr {($spi_cr1 >> 6) & 1}]   (1=enabled)"
puts "  MSTR bit2      = [expr {($spi_cr1 >> 2) & 1}]   (1=master)"
puts "SPI1_SR  = 0x[format %08X $spi_sr]"
puts "  TXE bit1 = [expr {($spi_sr >> 1) & 1}]  (1=ready)"
puts "  BSY bit7 = [expr {($spi_sr >> 7) & 1}]  (0=idle)"

resume
exit
