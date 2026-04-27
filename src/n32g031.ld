/* N32G031K8Q7-1 linker script — Vaporware SDK
 *
 * Flash: 64 KB total at 0x08000000
 *   Code region:  0x08000000 — 0x0800EFFF  (60 KB, used by linker)
 *   NV region:    0x0800F000 — 0x0800FFFF  (4 KB, reserved for nv.c)
 *
 * The linker sees only 60 KB of flash.  If your app exceeds this limit
 * the link will fail with "region FLASH overflowed" — that is intentional.
 * The top 4 KB (NV_FLASH_BASE in config.h) is written directly by nv.c
 * and must never be touched by the linker.
 *
 * SRAM: 8 KB at 0x20000000
 *   Stack: 2 KB at top (0x20001800–0x20001FFF)
 *   Data/BSS: remaining 6 KB
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 60K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 8K
}

ENTRY(Reset_Handler)

_estack = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* Sprite / bitmap data in flash */
    .sprites :
    {
        . = ALIGN(4);
        _sprites_start = .;
        *(.sprites)
        *(.sprites*)
        . = ALIGN(4);
        _sprites_end = .;
    } > FLASH

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > FLASH

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > SRAM

    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        *(.stack)
    } > SRAM

    /DISCARD/ :
    {
        libc.a ( * )
        libm.a ( * )
        libgcc.a ( * )
    }
}
