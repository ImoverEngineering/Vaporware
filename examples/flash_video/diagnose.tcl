catch {halt}
sleep 100
catch {halt}
sleep 50

set pc [mrw 0xE000EDF8]
set spi_cr1 [mrw 0x40013000]
set spi_sr [mrw 0x40013008]
set gpioa_moder [mrw 0x40010800]
set gpioa_odr [mrw 0x40010814]
set gpiob_moder [mrw 0x40010C00]
set gpiob_odr [mrw 0x40010C14]
set flash_sp [mrw 0x08000000]
set flash_rv [mrw 0x08000004]

puts "=== fw_dump.bin diagnostic ==="
puts "  PC          = 0x[format %08X $pc]"
puts "  Flash SP    = 0x[format %08X $flash_sp]"
puts "  Flash RV    = 0x[format %08X $flash_rv]"
puts "  SPI CR1     = 0x[format %08X $spi_cr1]"
puts "  SPI SR      = 0x[format %08X $spi_sr]"
puts "  GPIOA MODER = 0x[format %08X $gpioa_moder]"
puts "  GPIOA ODR   = 0x[format %08X $gpioa_odr]"
puts "  GPIOB MODER = 0x[format %08X $gpiob_moder]"
puts "  GPIOB ODR   = 0x[format %08X $gpiob_odr]"
puts "  PB4 BL      = [expr {($gpiob_odr >> 4) & 1}]  (0=backlight on, 1=off)"
puts "  PA15 CS     = [expr {($gpioa_odr >> 15) & 1}]  (should be 1=idle high)"
puts "  SPI BSY     = [expr {($spi_sr >> 7) & 1}]"
puts "  SPI TXE     = [expr {($spi_sr >> 1) & 1}]"

resume
