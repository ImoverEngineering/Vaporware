## debug_state.tcl — halt device and dump diagnostic state
## Run via: openocd -f n32g031.openocd.cfg -c "tcl_port disabled; telnet_port disabled; gdb_port disabled" \
##                  -c "init" -c "source debug_state.tcl" -c "exit"

init
adapter speed 500

# ── Halt (try 3 times) ───────────────────────────────────────────────────────
catch {halt}; sleep 30
catch {halt}; sleep 30
catch {halt}; sleep 50

puts ""
puts "══════════════════════════════════════════"
puts " N32G031 Live State Dump"
puts "══════════════════════════════════════════"

# ── CPU registers ─────────────────────────────────────────────────────────────
set pc   [expr {[mrw 0xE000EDF8] & 0xFFFFFFFE}]   ;# DCRSR/DCRDR would need halt — use reg instead
reg

puts ""
puts "── Core regs ──"
puts [format "  PC  = 0x%08X" [expr {[lindex [reg pc]  end]}]]
puts [format "  SP  = 0x%08X" [expr {[lindex [reg sp]  end]}]]
puts [format "  LR  = 0x%08X" [expr {[lindex [reg lr]  end]}]]

# ── RCC ───────────────────────────────────────────────────────────────────────
puts ""
puts "── RCC (clocks) ──"
set rcc_cr   [mrw 0x40021000]
set rcc_cfgr [mrw 0x40021004]
puts [format "  RCC_CR   = 0x%08X   (HSION=%d  HSIRDY=%d  PLLON=%d  PLLRDY=%d)" \
      $rcc_cr \
      [expr {($rcc_cr >> 0) & 1}] \
      [expr {($rcc_cr >> 1) & 1}] \
      [expr {($rcc_cr >> 24) & 1}] \
      [expr {($rcc_cr >> 25) & 1}]]
puts [format "  RCC_CFGR = 0x%08X   (SW=%d  SWS=%d  SWS_PLL_bit14=%d)" \
      $rcc_cfgr \
      [expr {($rcc_cfgr >> 0) & 3}] \
      [expr {($rcc_cfgr >> 2) & 3}] \
      [expr {($rcc_cfgr >> 14) & 1}]]

set rcc_apb1 [mrw 0x4002101C]
set rcc_apb2 [mrw 0x40021018]
puts [format "  APB1ENR  = 0x%08X   (TIM3EN=%d  PWREN=%d)" \
      $rcc_apb1 \
      [expr {($rcc_apb1 >> 1) & 1}] \
      [expr {($rcc_apb1 >> 28) & 1}]]
puts [format "  APB2ENR  = 0x%08X   (TIM1EN=%d  AFIOEN=%d  GPIOAEN=%d)" \
      $rcc_apb2 \
      [expr {($rcc_apb2 >> 11) & 1}] \
      [expr {($rcc_apb2 >> 0) & 1}] \
      [expr {($rcc_apb2 >> 2) & 1}]]

# ── RCC_CSR (reset cause flags — persist until RMVF bit cleared) ──────────────
puts ""
puts "── RCC_CSR (reset cause — READ THIS FIRST after any unexpected restart) ──"
set rcc_csr [mrw 0x40021024]
puts [format "  RCC_CTRLSTS (live) = 0x%08X" $rcc_csr]
puts [format "  PINRSTF=%d  PORRSTF=%d  SFTRSTF=%d  IWDGRSTF=%d  WWDGRSTF=%d  LPWRRSTF=%d" \
      [expr {($rcc_csr >> 3) & 1}] \
      [expr {($rcc_csr >> 4) & 1}] \
      [expr {($rcc_csr >> 5) & 1}] \
      [expr {($rcc_csr >> 6) & 1}] \
      [expr {($rcc_csr >> 7) & 1}] \
      [expr {($rcc_csr >> 8) & 1}]]

# Firmware writes RCC_CSR + magic at 0x20001FF0 on every main() entry.
puts ""
puts "── Saved reset cause (0x20001FF0, written by firmware at main() entry) ──"
set saved_csr   [mrw 0x20001FF0]
set saved_magic [mrw 0x20001FF4]
if {$saved_magic == 0xDEADBEEF} {
    puts [format "  Saved RCC_CTRLSTS = 0x%08X  (magic OK)" $saved_csr]
    puts [format "  PINRSTF=%d  PORRSTF=%d  SFTRSTF=%d  IWDGRSTF=%d  WWDGRSTF=%d  LPWRRSTF=%d" \
          [expr {($saved_csr >> 3) & 1}] \
          [expr {($saved_csr >> 4) & 1}] \
          [expr {($saved_csr >> 5) & 1}] \
          [expr {($saved_csr >> 6) & 1}] \
          [expr {($saved_csr >> 7) & 1}] \
          [expr {($saved_csr >> 8) & 1}]]
} else {
    puts [format "  Magic = 0x%08X (expected 0xDEADBEEF) — firmware hasn't run yet or wrong build" $saved_magic]
}
puts "  Flags: LPWR=low-power  WWDG=window-wdog  IWDG=indep-wdog  SFT=software"
puts "         BOR=brown-out   PIN=NRST-pin      OBL=option-byte-load"
puts "  NOTE: live CSR flags may be 0 if SRST was used to connect (clears flags)"

# ── TIM3 ─────────────────────────────────────────────────────────────────────
puts ""
puts "── TIM3 (delay_ms timebase) ──"
set tim3_cr1 [mrw 0x40000400]
set tim3_psc [mrw 0x40000428]
set tim3_arr [mrw 0x4000042C]
set tim3_cnt [mrw 0x40000424]
set tim3_sr  [mrw 0x40000410]
puts [format "  CR1=%08X  PSC=%d  ARR=%d  CNT=%d  SR=%08X   (CEN=%d  UIF=%d)" \
      $tim3_cr1 $tim3_psc $tim3_arr $tim3_cnt $tim3_sr \
      [expr {($tim3_cr1 >> 0) & 1}] \
      [expr {($tim3_sr  >> 0) & 1}]]

# ── TIM1 ─────────────────────────────────────────────────────────────────────
puts "── TIM1 (ms_now) ──"
set tim1_cr1 [mrw 0x40012C00]
set tim1_psc [mrw 0x40012C28]
set tim1_cnt [mrw 0x40012C24]
set tim1_sr  [mrw 0x40012C10]
puts [format "  CR1=%08X  PSC=%d  CNT=%d  SR=%08X   (CEN=%d)" \
      $tim1_cr1 $tim1_psc $tim1_cnt $tim1_sr \
      [expr {($tim1_cr1 >> 0) & 1}]]

# ── EXTI ─────────────────────────────────────────────────────────────────────
puts ""
puts "── EXTI (sleep wake source) ──"
set exti_imr  [mrw 0x40010400]
set exti_emr  [mrw 0x40010404]
set exti_ftsr [mrw 0x4001040C]
set exti_pr   [mrw 0x40010414]
puts [format "  IMR=0x%08X  EMR=0x%08X  FTSR=0x%08X  PR=0x%08X" \
      $exti_imr $exti_emr $exti_ftsr $exti_pr]
puts [format "  EXTI7 → IMR=%d EMR=%d FTSR=%d PR=%d" \
      [expr {($exti_imr  >> 7) & 1}] \
      [expr {($exti_emr  >> 7) & 1}] \
      [expr {($exti_ftsr >> 7) & 1}] \
      [expr {($exti_pr   >> 7) & 1}]]

# ── PWR + SCR ─────────────────────────────────────────────────────────────────
puts ""
puts "── Power / Sleep ──"
set pwr_cr [mrw 0x40007000]
set scr    [mrw 0xE000ED10]
puts [format "  PWR_CR=0x%08X   (LPDS=%d  PDDS=%d)" \
      $pwr_cr \
      [expr {($pwr_cr >> 0) & 1}] \
      [expr {($pwr_cr >> 1) & 1}]]
puts [format "  SCR   =0x%08X   (SLEEPDEEP=%d  SEVONPEND=%d)" \
      $scr \
      [expr {($scr >> 2) & 1}] \
      [expr {($scr >> 4) & 1}]]

# ── GPIOA ─────────────────────────────────────────────────────────────────────
puts ""
puts "── GPIOA (PA4=VCC_FET  PA5=COIL!  PA6=VCC_FET  PA7=BTN) ──"
set gpioa_moder [mrw 0x40010800]
set gpioa_odr   [mrw 0x40010814]
set gpioa_idr   [mrw 0x40010810]
puts [format "  MODER=0x%08X  ODR=0x%08X  IDR=0x%08X" \
      $gpioa_moder $gpioa_odr $gpioa_idr]
puts [format "  PA4=%d PA5=%d PA6=%d PA7=%d (IDR bits)" \
      [expr {($gpioa_idr >> 4) & 1}] \
      [expr {($gpioa_idr >> 5) & 1}] \
      [expr {($gpioa_idr >> 6) & 1}] \
      [expr {($gpioa_idr >> 7) & 1}]]

# ── IWDG ─────────────────────────────────────────────────────────────────────
puts ""
puts "── IWDG ──"
set iwdg_sr  [mrw 0x4000300C]
set iwdg_pr  [mrw 0x40003004]
set iwdg_rlr [mrw 0x40003008]
puts [format "  PR=%d  RLR=%d  SR=0x%08X" $iwdg_pr $iwdg_rlr $iwdg_sr]

# ── SRAM sentinel area (0x20000040..0x20000060) ───────────────────────────────
puts ""
puts "── SRAM sentinels (0x20000040) ──"
mdw 0x20000040 8

# ── Stack dump ────────────────────────────────────────────────────────────────
puts ""
puts "── Stack top (SP - 64 bytes) ──"
set sp_val [expr {[lindex [reg sp] end]}]
set sp_base [expr {$sp_val & 0xFFFFFFE0}]
puts [format "  SP=0x%08X  dumping from 0x%08X" $sp_val $sp_base]
mdw $sp_base 16

puts ""
puts "══════════════════════════════════════════"
puts " Dump complete — device remains halted."
puts "══════════════════════════════════════════"
