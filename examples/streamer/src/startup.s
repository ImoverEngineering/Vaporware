/* N32G031K8Q7-1 startup file — ARM Cortex-M0, 64KB flash, 8KB SRAM */

    .syntax unified
    .cpu cortex-m0
    .thumb

/* ---- Vector table ---- */
/* _estack comes from the linker script: ORIGIN(SRAM)+LENGTH(SRAM) = 0x20002000.
 * The stack grows downward from 0x20002000.  The streaming buffers occupy
 * 0x20000100–0x20001107, well below the stack — no overlap. */
    .section .isr_vector, "a"
    .align 2
    .global _vectors
_vectors:
    .word   _estack                 /* 0: Initial SP */
    .word   Reset_Handler           /* 1: Reset */
    .word   NMI_Handler             /* 2: NMI */
    .word   HardFault_Handler       /* 3: HardFault */
    .word   0                       /* 4: Reserved */
    .word   0                       /* 5: Reserved */
    .word   0                       /* 6: Reserved */
    .word   0                       /* 7: Reserved */
    .word   0                       /* 8: Reserved */
    .word   0                       /* 9: Reserved */
    .word   0                       /* 10: Reserved */
    .word   SVC_Handler             /* 11: SVCall */
    .word   0                       /* 12: Reserved */
    .word   0                       /* 13: Reserved */
    .word   PendSV_Handler          /* 14: PendSV */
    .word   SysTick_Handler         /* 15: SysTick */
    /* External interrupts */
    .word   Default_Handler         /* 16: WWDG */
    .word   Default_Handler         /* 17: PVD */
    .word   Default_Handler         /* 18: RTC */
    .word   Default_Handler         /* 19: Flash */
    .word   Default_Handler         /* 20: RCC */
    .word   Default_Handler         /* 21: EXTI0_1 */
    .word   Default_Handler         /* 22: EXTI2_3 */
    .word   Default_Handler         /* 23: EXTI4_15 */
    .word   Default_Handler         /* 24: Reserved */
    .word   Default_Handler         /* 25: DMA_CH1 */
    .word   Default_Handler         /* 26: DMA_CH2_3 */
    .word   Default_Handler         /* 27: DMA_CH4_5 */
    .word   Default_Handler         /* 28: ADC */
    .word   Default_Handler         /* 29: TIM1 BRK/UP */
    .word   Default_Handler         /* 30: TIM1 CC */
    .word   Default_Handler         /* 31: TIM2 */
    .word   Default_Handler         /* 32: TIM3 */
    .word   Default_Handler         /* 33: TIM6 */
    .word   Default_Handler         /* 34: Reserved */
    .word   Default_Handler         /* 35: TIM14 */
    .word   Default_Handler         /* 36: TIM15 */
    .word   Default_Handler         /* 37: TIM16 */
    .word   Default_Handler         /* 38: TIM17 */
    .word   Default_Handler         /* 39: I2C1 */
    .word   Default_Handler         /* 40: I2C2 */
    .word   Default_Handler         /* 41: SPI1 */
    .word   Default_Handler         /* 42: SPI2 */
    .word   Default_Handler         /* 43: USART1 */
    .word   Default_Handler         /* 44: USART2 */

/* ---- Reset handler ---- */
    .section .text.Reset_Handler, "ax"
    .global Reset_Handler
    .type Reset_Handler, %function
Reset_Handler:
    /* Copy .data from flash to SRAM */
    ldr     r0, =_sdata
    ldr     r1, =_edata
    ldr     r2, =_sidata
    movs    r3, #0
    b       data_loop_check
data_loop:
    ldr     r4, [r2, r3]
    str     r4, [r0, r3]
    adds    r3, r3, #4
data_loop_check:
    adds    r4, r0, r3
    cmp     r4, r1
    bcc     data_loop

    /* Zero .bss */
    ldr     r0, =_sbss
    ldr     r1, =_ebss
    movs    r2, #0
    b       bss_loop_check
bss_loop:
    str     r2, [r0]
    adds    r0, r0, #4
bss_loop_check:
    cmp     r0, r1
    bcc     bss_loop

    /* Call main */
    bl      main
    b       .

/* ---- Default weak handlers ---- */
    .weak   NMI_Handler
    .thumb_set NMI_Handler, Default_Handler

    .weak   HardFault_Handler
    .thumb_set HardFault_Handler, Default_Handler

    .weak   SVC_Handler
    .thumb_set SVC_Handler, Default_Handler

    .weak   PendSV_Handler
    .thumb_set PendSV_Handler, Default_Handler

    .weak   SysTick_Handler
    .thumb_set SysTick_Handler, Default_Handler

    .weak   TIM3_IRQHandler
    .thumb_set TIM3_IRQHandler, Default_Handler

    .section .text.Default_Handler, "ax"
    .type   Default_Handler, %function
Default_Handler:
    b       .

    /* .size not needed — removed to avoid assembler issues */
