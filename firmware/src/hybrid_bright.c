/*
 * "Hybrid" daylight brightness for yeelink.light.ceiling3 2.0.2_0048 - with fail-safe.
 *
 * Problem (measured, decomp/LIGHT_MIXER.md): the main LED driver is off below ~5.5 % duty
 * and stays on one constant minimum level up to ~10.5-11 % (~= moonlight at nl_br 100).
 * The daylight channel therefore cannot go below that level, the moonlight LEDs can
 * (nl_br N = N % duty, linear).
 *
 * Hook: BL @0x100c09ce (next_trans -> start_transition 0x100c07d4, the ONLY caller of
 * start_transition, decomp/LIGHT_UNIT.md) is redirected here. Every daylight transition
 * (set_bright, set_ct, power on/off, remote +/-, scenes, restore after the wall switch,
 * colour-flow steps) passes through it.
 *
 *   dl bright 1..K-1 : dl target forced to 0, moonlight fades to nl(b) = round(100 b^2/(K-1)^2)
 *   dl bright K..100 : moonlight fades out, dl gets b' = 1 + round((b-K)*99/(100-K)); the mixer
 *                      floor is raised to the driver knee (10 %) by patch_ota.py, so bright K is
 *                      the driver's minimum and 100 stays 100 %.
 *   real moonlight mode (unit->active == 1) : untouched.
 * Unit/channel state is not modified: the app sees active_mode 0 and bright b; saved state
 * stays "daylight @ b", so power-on returns to the same hybrid level.
 *
 * FAIL-SAFE (pass-through = exactly the original call, i.e. the proven "patched" firmware):
 *  1. Placement: both parts live INSIDE the original segment-0 image, in two slots of dead
 *     code (no image growth, nothing depends on how the bootloader handles a bigger segment):
 *       A 0x100ecac4..0x100ecb98 (212 B) FUN_100ecac4: CT interpolate, installed by
 *         FUN_100ecc00 and immediately overwritten by FUN_100ecc40 -> never called;
 *       B 0x100ec204..0x100ec2f0 (236 B) FUN_100ec204: thermal derate hook, only called when
 *         mixer+0x0e is set, which the firmware never does.
 *     Whole-image scan: no other code branches into or loads literals from either slot.
 *  2. Crash latch: every fault handler of the stock firmware ends in arch_crash_reboot(),
 *     which stores reboot reason 2 ("crash") in flash; yglc_reboot_restore() reads it back
 *     on the next boot into g_warm_boot_type (u8 @0x20008084). While that byte is 2 - or the
 *     RAM crash latch of blackbox_monitor.c is set, in case that flash write did not make it
 *     (bb_boot_follows_crash(), bb_shared.h) - the hook is a pure pass-through for the whole
 *     boot -> a crash caused here can happen at most once per power cycle, the lamp always
 *     comes back usable and OTA-reachable.
 *     (patch_ota.py 0x100ca2c4 only makes that reboot immediate: it skips the stock dump of
 *     ~320 KB of RAM to the UART, which takes minutes; see blackbox_monitor.c.)
 *  3. Sanity: NULL settings, unit, channel or effect-manager pointers -> pass-through.
 */
#include "../include/bb_shared.h"
#include "../include/yglc_light_mixer_clean.h"
#include "../include/yglc_light_unit_clean.h"

#ifndef HYBRID_K
#define HYBRID_K 10u
#endif
_Static_assert(HYBRID_K >= 2u && HYBRID_K <= 50u, "HYBRID_K range");

/* original 0x100c07d4 (PROVIDEd by ceiling3_symbols.ld) */
int FUN_100c07d4(yglc_trans_manager_t *tm, yglc_trans_tuple_t *t, const yglc_trans_cbs_t *cbs);

#define HYB_SETTINGS   (*(const uint8_t *const volatile *)0x20006908u)   /* g_yglc_settings_ptr */

/*
 * Slot A: start a transition on `tm` with the tuple `t`, but with bright = `level`
 * (t->bright restored afterwards; start_transition copies *t into tm->cur). Used for the
 * moonlight channel with a daylight tuple: for the nl single-channel mixer only bright and
 * duration matter (ct is ignored, both min_bright values of the nl trans manager are 0).
 */
__attribute__((section(".text.hybrid_a"), noinline, used))
int hybrid_start_with(yglc_trans_manager_t *tm, yglc_trans_tuple_t *t,
                      const yglc_trans_cbs_t *cbs, uint32_t level)
{
    uint8_t keep = t->bright;
    t->bright = (uint8_t)level;
    int r = FUN_100c07d4(tm, t, cbs);
    t->bright = keep;
    return r;
}

/*
 * Slot B: a moonlight transition requested by the firmware while daylight is active:
 * nl OFF while the hybrid range is shown keeps the hybrid level; anything else unchanged.
 */
__attribute__((section(".text.hybrid_b"), noinline, used))
int hybrid_nl(yglc_trans_manager_t *nl_tm, yglc_trans_tuple_t *t, const yglc_trans_cbs_t *cbs,
              uint32_t keep)
{
    if (t->bright == 0u && keep != 0u) {
        return hybrid_start_with(nl_tm, t, cbs, keep);
    }
    return FUN_100c07d4(nl_tm, t, cbs);
}

/* Slot B: daylight transition (hybrid range below K, rescaling from K up). */
__attribute__((section(".text.hybrid_b"), noinline, used))
int hybrid_dl(yglc_trans_manager_t *dl_tm, yglc_trans_tuple_t *t, const yglc_trans_cbs_t *cbs,
              yglc_trans_manager_t *nl_tm)
{
    uint8_t b = t->bright;
    uint32_t was = dl_tm->_unk_dc;
    uint32_t nl = 0u;
    uint8_t nb = b;
    int r;

    if (b != 0u && b < HYBRID_K) {
        const uint32_t d = (HYBRID_K - 1u) * (HYBRID_K - 1u);
        nl = (100u * b * b + d / 2u) / d;
        nl = (nl == 0u) ? 1u : (nl > 100u ? 100u : nl);
        nb = 0u;                                /* dl fades to dark */
    } else if (b >= HYBRID_K) {
        nb = (uint8_t)(1u + ((b - HYBRID_K) * 99u + (100u - HYBRID_K) / 2u) / (100u - HYBRID_K));
    }
    dl_tm->_unk_dc = nl;

    t->bright = nb;
    r = FUN_100c07d4(dl_tm, t, cbs);            /* copies *t into tm->cur, sets t->scale */
    t->bright = b;                              /* the effect keeps the user's value */

    if (nl != 0u || was != 0u) {
        /* nl effect callbacks stay as they are (memcpy onto itself in start_transition) */
        (void)hybrid_start_with(nl_tm, t, &nl_tm->cbs, nl);
    }
    return r;
}

/* Slot A: the hook itself. */
__attribute__((section(".text.hybrid_a"), used))
int hybrid_start_transition(yglc_trans_manager_t *tm, yglc_trans_tuple_t *t,
                            const yglc_trans_cbs_t *cbs)
{
    const uint8_t *s = HYB_SETTINGS;
    yglc_light_unit_t *u;
    yglc_trans_manager_t *dl_tm, *nl_tm;

    if (bb_boot_follows_crash() || s == NULL) {
        return FUN_100c07d4(tm, t, cbs);
    }
    u = *(yglc_light_unit_t *const *)(s + 0x58u);
    if (u == NULL || u->channel[0] == NULL || u->channel[1] == NULL ||
        u->channel[0]->effects == NULL || u->channel[1]->effects == NULL) {
        return FUN_100c07d4(tm, t, cbs);
    }
    dl_tm = u->channel[0]->effects->trans;
    nl_tm = u->channel[1]->effects->trans;
    if (dl_tm == NULL || nl_tm == NULL) {
        return FUN_100c07d4(tm, t, cbs);
    }
    if (u->active != 0) {                       /* real moonlight mode: hands off */
        dl_tm->_unk_dc = 0u;
        return FUN_100c07d4(tm, t, cbs);
    }

    if (tm == nl_tm) {
        return hybrid_nl(tm, t, cbs, dl_tm->_unk_dc);
    }
    if (tm != dl_tm) {
        return FUN_100c07d4(tm, t, cbs);
    }
    return hybrid_dl(dl_tm, t, cbs, nl_tm);
}
