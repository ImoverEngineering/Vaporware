# pa456_test.tcl — individually pulse PA4, PA5, PA6 HIGH for 1 s each.
# Run while vape is connected. Watch/feel for coil activation to identify
# which pin is the coil gate. Others should be VCC FET gate or ADC sense.
#
# Usage: openocd -f n32g031.openocd.cfg -c "source pa456_test.tcl" -c exit

init
adapter speed 500
catch {halt}
sleep 100
catch {halt}
sleep 50

# Enable GPIOA clock
set apb2 [mrw 0x40021018]
mww 0x40021018 [expr {$apb2 | (1 << 2)}]
sleep 5

set GPIOA_MODER 0x40010800
set GPIOA_BSRR  0x40010818

proc test_pa {pin} {
    global GPIOA_MODER GPIOA_BSRR

    # Save and configure pin as output
    set m_orig [mrw $GPIOA_MODER]
    set m_new  [expr {($m_orig & ~(3 << ($pin * 2))) | (1 << ($pin * 2))}]

    # Ensure pin is LOW first (safe state)
    mww $GPIOA_BSRR [expr {1 << ($pin + 16)}]
    mww $GPIOA_MODER $m_new
    sleep 200

    puts ">>> PA$pin HIGH for 1 s — coil firing?"
    mww $GPIOA_BSRR [expr {1 << $pin}]
    sleep 1000

    # Drive LOW then restore mode
    mww $GPIOA_BSRR [expr {1 << ($pin + 16)}]
    mww $GPIOA_MODER $m_orig
    puts "    PA$pin back LOW, mode restored"
    sleep 1000
}

puts ""
puts "========================================="
puts " PA4 / PA5 / PA6 coil-pin identification "
puts "========================================="
puts ""
puts "Each pin will go HIGH for 1 s with a pause between."
puts "Note which one fires the coil."
puts ""

test_pa 4
test_pa 5
test_pa 6

puts ""
puts "Done. Which pin fired the coil?"
exit
