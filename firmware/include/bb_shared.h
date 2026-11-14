#pragma once
/*
 * Layout shared by blackbox_monitor.c (owner) and hybrid_bright.c (reads the crash latch).
 * The black-box RAM block is "noinit": above .bss (ends 0x2003AA44), 16 KB below the MSP
 * stack top 0x20040000, outside the FreeRTOS heap (0x20008f98..0x20038798); nothing in the
 * stock firmware references 0x2003AA44..0x20040000 and the startup code does not clear it.
 */
#include <stdint.h>

#define BB_ADDR          0x2003C000u
#define BB_MAGIC         0x32424259u   /* "YBB2" at +0, ~BB_MAGIC at +4 */
#define BB_CRASH_OFF     8u            /* u32 crash latch: */
#define BB_CRASHED       0x53415243u   /*   "CRAS" written by bb_on_crash() in fault context    */
#define BB_CRASH_SEEN    0x4e454553u   /*   "SEEN" set by bb_boot(): this boot follows a crash  */

/* .bss words of the stock yglc_network_monitor, reused by the monitor. [1] is 0 from reset
 * until the first monitor tick of a boot (bb_boot), non-zero afterwards. */
#define BB_MON_ST        ((volatile uint32_t *)0x20008030u)

#define BB_BOOT_TYPE     (*(const volatile uint8_t *)0x20008084u)   /* g_warm_boot_type: 2 = crash */

/*
 * 1 if this boot follows a crash. The stock source of truth is g_warm_boot_type == 2 (written
 * to flash by arch_crash_reboot from the fault handler); the RAM latch covers the case where
 * that flash write did not make it. BB_CRASH_SEEN left over from an earlier boot is ignored
 * until bb_boot() of this boot has run (BB_MON_ST[1] != 0).
 */
static inline int bb_boot_follows_crash(void)
{
    const volatile uint32_t *bb = (const volatile uint32_t *)BB_ADDR;
    if (BB_BOOT_TYPE == 2u) {
        return 1;
    }
    if (bb[0] != BB_MAGIC || bb[1] != ~BB_MAGIC) {
        return 0;
    }
    uint32_t latch = bb[BB_CRASH_OFF / 4u];
    return latch == BB_CRASHED || (latch == BB_CRASH_SEEN && BB_MON_ST[1] != 0u);
}
