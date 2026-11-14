/*
 * Linker script for the zero-BSS in-place yglc_network_monitor replacement
 * mapped directly over the original 224-byte slot [0x100bd918 .. 0x100bd9f8).
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)

INCLUDE "ceiling3_symbols.ld"

MEMORY
{
    INPLACE_SLOT (rx) : ORIGIN = 0x100bd918, LENGTH = 0x000000E0 /* 224 bytes */
}

ENTRY(ceiling3_yglc_network_monitor_inplace)

SECTIONS
{
    .text : ALIGN(2)
    {
        KEEP(*(.text.ceiling3_inplace_monitor))
        *(.rodata*)
    } > INPLACE_SLOT

    /DISCARD/ :
    {
        *(.text*)
        *(.data*)
        *(.bss*)
        *(COMMON)
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
        *(.note*)
    }
}
