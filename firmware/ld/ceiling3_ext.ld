/*
 * Linker script for compiling C11 Cortex-M4F firmware extensions against
 * yeelink.light.ceiling3 2.0.2_0048 segment-0 ROM symbols.
 *
 * segment-0 ends at 0x1015064c; miio_psm begins at 0x1fc000 (XIP 0x101fc000).
 * NOTE: heap_4 ucHeap covers 0x20008f98..0x20038798 and FreeRTOS static RTOS
 * buffers occupy 0x20038798..0x2003AA44 (sysram_bss). Any RAM-stateful overlay
 * using EXT_SRAM must first relocate free SRAM or use zero-BSS hooks (such as
 * ceiling3_inplace_monitor.ld at 0x100bd918 using yglc_network_monitor's existing
 * 8-byte slot 0x20008030..0x20008037).
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)

INCLUDE "ceiling3_symbols.ld"

MEMORY
{
    EXT_FLASH (rx)  : ORIGIN = 0x10150650, LENGTH = 0x000AB9B0
    EXT_SRAM  (rwx) : ORIGIN = 0x20039800, LENGTH = 0x00001200
}

ENTRY(ceiling3_network_monitor_hook)

SECTIONS
{
    .text : ALIGN(4)
    {
        KEEP(*(.text.ceiling3_network_monitor_hook))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > EXT_FLASH

    .data : ALIGN(4)
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > EXT_SRAM AT > EXT_FLASH
    _sidata = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > EXT_SRAM

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
        *(.note*)
    }
}
