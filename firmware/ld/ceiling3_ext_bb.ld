/*
 * blackbox_monitor.c appended after the stock segment-0 image (ends 0x1015064c); the
 * segment-0 partition reaches 0x1a1000. Zero BSS / zero data: RAM is the fixed noinit block
 * at 0x2003C000 and the monitor's .bss words at 0x20008030 (see the source).
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)

INCLUDE "ceiling3_symbols.ld"


MEMORY
{
    EXT (rx) : ORIGIN = 0x10150650, LENGTH = 0x1000
}

ENTRY(bb_monitor_tick)

SECTIONS
{
    .ext : ALIGN(4)
    {
        __ext_start = .;
        KEEP(*(.text.bb_monitor_tick))
        KEEP(*(.text.bb_on_crash))
        *(.text*)
        *(.rodata*)
        __ext_end = .;
    } > EXT

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
