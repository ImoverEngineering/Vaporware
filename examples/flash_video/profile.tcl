# profile.tcl — sample PC 10 times to find hotspot
halt
sleep 30
catch {halt}

set pcs {}
for {set i 0} {$i < 10} {incr i} {
    resume
    sleep 8
    catch {halt}
    sleep 5
    catch {halt}
    # Read PC from DHCSR/DPC debug register
    set dpc [mrw 0xE000101C]
    lappend pcs [format "0x%08X" $dpc]
}

puts "=== PC samples (10x) ==="
foreach p $pcs { puts "  $p" }

# Ranges:
# flash_spi_xfer: 0x08000814 - 0x0800085C
# main/write_frame: 0x08000860 - 0x08000A44
# TXE wait inner loop: 0x080009AC - 0x080009B2
set n_bitbang 0
set n_txe_wait 0
set n_other 0
foreach p $pcs {
    set addr [expr {[scan $p %i addr]; $addr}]
    if {$addr >= 0x08000814 && $addr <= 0x0800085C} {
        incr n_bitbang
    } elseif {$addr >= 0x080009AC && $addr <= 0x080009B2} {
        incr n_txe_wait
    } else {
        incr n_other
    }
}
puts "In flash_spi_xfer (bit-bang): $n_bitbang / 10"
puts "In TXE wait loop:             $n_txe_wait / 10"
puts "Other (write_frame, etc):     $n_other / 10"

# Key registers
set spi_cr1 [mrw 0x40012000]
puts "SPI1_CR1 = 0x[format %08X $spi_cr1]  BR=[expr {($spi_cr1>>3)&7}]"
puts "TIM3_PSC = [mrw 0x40000428]  (47=48MHz 7=8MHz)"
puts "Sentinel: 0x[format %08X [mrw 0x20000040]]  (AA001111=PLL ok)"
puts "CFGR snapshot: 0x[format %08X [mrw 0x20000050]]  (bit14=PLL as SYSCLK)"

resume
exit
