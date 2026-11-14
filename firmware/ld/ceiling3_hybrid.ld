/*
 * hybrid_bright.c placed in-place into two slots of dead code of the original segment 0
 * (see the fail-safe notes in firmware/src/hybrid_bright.c):
 *   SLOT_A 0x100ecac4..0x100ecb98 (212 B)  hybrid_start_transition (BL target @0x100c09ce)
 *   SLOT_B 0x100ec204..0x100ec2f0 (236 B)  daylight path, nl path
 * Zero BSS / zero data: everything must be .text / .rodata.
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)

INCLUDE "ceiling3_symbols.ld"


MEMORY
{
    SLOT_B (rx) : ORIGIN = 0x100ec204, LENGTH = 236
    SLOT_A (rx) : ORIGIN = 0x100ecac4, LENGTH = 212
}

ENTRY(hybrid_start_transition)

SECTIONS
{
    .slot_b : ALIGN(4)
    {
        KEEP(*(.text.hybrid_b))
    } > SLOT_B

    .slot_a : ALIGN(4)
    {
        KEEP(*(.text.hybrid_a))
        *(.text*)
        *(.rodata*)
    } > SLOT_A

    /DISCARD/ :
    {
        *(.data*)
        *(.bss*)
        *(COMMON)
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
        *(.note*)
    }
}
