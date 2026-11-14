/*
 * Clean C11 reconstruction of the light mixer library and the PWM output path of the
 * Yeelight Ceiling3 firmware (yeelink.light.ceiling3 2.0.2_0048, MT7697 Cortex-M4F).
 *
 * Source artifacts: decomp/wifi/wpa_supplicant_wifi.c (Ghidra mis-filed the mixer there,
 * 0x100ebf70..0x100ecc70), decomp/app/yglc_core.c (0x100c0272, 0x100c02bc, 0x100c0616),
 * decomp/hal/mt7687_hal.c (hal_pwm_*). Constants were read from the flash image.
 *
 * The firmware does the easing math in soft double precision (__aeabi_d*); it is written
 * here with double so rounding matches. Float->uint conversions use VCVT.U32.F32, i.e.
 * truncation with saturation (negative -> 0): see yglc_f2u().
 *
 * Findings and quirks are marked "NOTE".
 */
#include "../include/yglc_light_mixer_clean.h"

/* ---- MT7687 HAL (resolved by ceiling3_symbols.ld) ---------------------------------- */
int  hal_pwm_init(uint8_t source_clock);                                    /* 0x1007343c */
int  hal_pwm_set_frequency(uint32_t ch, uint32_t freq_hz, uint32_t *total); /* 0x100734ac */
int  hal_pwm_set_duty_cycle(uint32_t ch, uint32_t duty_ticks);              /* 0x1007355c */
int  hal_pwm_start(uint32_t ch);                                            /* 0x10073608 */
int  hal_pwm_get_running_status(uint32_t ch, uint8_t *running);             /* 0x1007367c */
int  hal_gpio_init(uint32_t gpio);                                          /* 0x100732ce (stub) */
int  hal_pinmux_set_function(uint32_t gpio, uint32_t func);                 /* 0x100732ac */
int  hal_gpt_delay_ms(uint32_t ms);                    /* 0x100733b0 (assumed: GPT delay) */
void *pvPortMalloc(size_t size);

/* yglc timer service (yglc_core.c) */
int  yglc_timer_add(void *service, void *timer);
int  yglc_timer_del(void *timer);
int  yglc_timer_is_running(void *timer);
int  log_printf(const char *fmt, ...);
extern void *g_yglc_timer_service;            /* *(void **)0x20008020 */
extern uint32_t g_yglc_trans_scratch;         /* 0x2000804c: per-channel "hi" scratch */

/* Literal constants of the original code */
#define K_PCT        100.0f   /* 0x100ec1f8 / 0x100ec69c / 0x100ec93c / 0x100eca2c        */
#define K_ROUND      0.49f    /* 0x100ec718 / 0x100ec944: rounding before VCVT            */
#define K_QUAD_KNEE  15.0f    /* FUN_100ec140 quadratic-curve smoothing offset            */
#define K_DERATE_HOT 90.0f    /* 0x100ec2dc  start derating at >= 90 C                    */
#define K_DERATE_OK  75.0f    /* 0x100ec2e8  recover below 75 C                           */
#define K_DERATE_K   0.003f   /* 0x100ec2e0                                               */
#define K_DERATE_MIN 0.6f     /* 0x100ec2e4                                               */
#define K_DERATE_UP  0.02f    /* 0x100ec2ec                                               */

static uint32_t yglc_f2u(float v)
{
    if (!(v > 0.0f)) {
        return 0u;            /* VCVT.U32.F32 saturates negatives and NaN to 0 */
    }
    if (v >= 4294967295.0f) {
        return 0xffffffffu;
    }
    return (uint32_t)v;
}

/* ===================================================================================== */
/* Math helpers                                                                           */
/* ===================================================================================== */

/* 0x100ebf70: quadratic Lagrange interpolation through 3 points. */
float yglc_lagrange3(float x, float x0, float y0, float x1, float y1, float x2, float y2)
{
    return (y0 * (x - x1) * (x - x2)) / ((x0 - x1) * (x0 - x2)) +
           (y1 * (x - x2) * (x - x0)) / ((x1 - x2) * (x1 - x0)) +
           (y2 * (x - x0) * (x - x1)) / ((x2 - x0) * (x2 - x1));
}

/* 0x100ebfce: index of the row with row.ct <= ct < next.ct; 0 if the end is reached. */
uint32_t yglc_ct_find_row(const yglc_ct_calib_t *t, uint32_t ct_k)
{
    for (uint8_t i = 0;; i++) {
        uint32_t lo = t->row[i].ct_k;
        if (lo > 9999u) {
            return 0u;
        }
        if (lo <= ct_k && ct_k < t->row[i + 1].ct_k) {
            return i;
        }
    }
}

/*
 * 0x100ec140: bright (1..100) -> scale 0..1.
 *   LINEAR    : m == 0 -> b/100; else straight line from m% at b=1 to 100% at b=100
 *               (Ghidra form: ((100-100m)/-99 + b*(m-100)/-99) / 100).
 *   QUADRATIC : k = max(m, 15); f = (b+k)/(k+100); g = ((k+1)/(k+100))^2;
 *               (m + (100-m)*(f^2-g)/(1-g)) / 100  -> m% at b=1, 100% at b=100.
 * The original uses fused VFMA/VFMS/VFNMS (single rounding) - reproduced with __builtin_fmaf().
 * NOTE: with QUADRATIC and m == 0, b=1 gives only an FMA rounding residue (~1e-9, sign
 *       depends on k): practically 0, but a positive residue is lifted to the floor by the
 *       remap. The curve needs m >= 1 to be usable.
 */
float yglc_mixer_bright_scale(const yglc_mixer_t *m, uint32_t bright)
{
    if (bright == 0u) {
        return 0.0f;
    }
    float mn = (float)m->min_bright_pct;
    float b = (float)bright;
    if (m->bright_curve == YGLC_BRIGHT_CURVE_LINEAR) {
        if (mn != 0.0f) {
            float a = __builtin_fmaf(-mn, K_PCT, K_PCT) / -99.0f;          /* vfms + vdiv */
            b = __builtin_fmaf(b, (mn - K_PCT) / -99.0f, a);               /* vfma        */
        }
        return b / K_PCT;
    }
    if (m->bright_curve == YGLC_BRIGHT_CURVE_QUADRATIC) {
        float k = (mn > K_QUAD_KNEE) ? mn : K_QUAD_KNEE;
        float den = k + K_PCT;
        float f = (b + k) / den;
        float g = (k + 1.0f) / den;
        g = g * g;
        float d = __builtin_fmaf(f, f, -g);                                 /* vfnms       */
        return (mn + ((K_PCT - mn) * d) / (1.0f - g)) / K_PCT;
    }
    return 0.0f;
}

/*
 * 0x100ec654: floor remap. Any non-zero value v in [0, max] is mapped affinely into
 * [floor% * max, max]:  v' = (F + (1 - F) * v/max) * max,  F = floor_pct/100.
 * floor_pct == 0 disables it. This is the "8 %" floor of the daylight channel.
 */
void yglc_mixer_floor_remap(float floor_pct, const yglc_mixer_t *m, float *value)
{
    if (floor_pct == 0.0f) {
        return;
    }
    float max = (float)m->max_pwm;
    float rel = *value / max;
    if (rel != 0.0f) {
        float f = floor_pct / K_PCT;
        *value = __builtin_fmaf(-(f - 1.0f), rel, f) * max;   /* vfms: single rounding */
    } else {
        *value = 0.0f;
    }
}

/*
 * 0x100ec2f0: value of one channel at `step` of m->trans_steps.
 *   LINEAR        : from + (to - from) * step / total
 *   CUBIC_LOW_END : cubic ease anchored at the smaller endpoint (rising: lo + d*(s/T)^3,
 *                   falling: lo + d*((T-s)/T)^3) - slow where the light is dim
 *   CUBIC_FROM    : from + (to - from) * (s/T)^3
 * The result never moves backwards: rising -> >= prev_out, falling -> <= prev_out.
 */
uint32_t yglc_mixer_trans_ease(const yglc_mixer_t *m, uint32_t from, uint32_t to,
                               uint32_t step, uint32_t prev_out)
{
    uint32_t total = m->trans_steps;
    if ((double)step == (double)total) {
        return to;
    }
    double v;
    switch (m->trans_curve) {
    case YGLC_TRANS_CURVE_LINEAR:
        v = (double)from + ((double)to - (double)from) * (double)step / (double)total;
        break;
    case YGLC_TRANS_CURVE_CUBIC_LOW_END: {
        double lo, x, hi;
        if (to < from) {
            lo = (double)to;
            x = (double)((float)total - (float)step);
            hi = (double)from;
        } else {
            lo = (double)from;
            x = (double)(float)step;
            hi = (double)to;
        }
        float t3 = (float)total * (float)total * (float)total;
        v = (hi - lo) / (double)t3 * x * x * x + lo;
        break;
    }
    case YGLC_TRANS_CURVE_CUBIC_FROM: {
        double x = (double)(float)step;
        float t3 = (float)total * (float)total * (float)total;
        v = ((double)to - (double)from) / (double)t3 * x * x * x + (double)from;
        break;
    }
    default:
        return 0u;
    }
    uint32_t r = (v <= 0.0) ? 0u : (uint32_t)v;
    if (to < from) {
        if (prev_out <= r) {
            r = prev_out;
        }
    } else if (r < prev_out) {
        r = prev_out;
    }
    return r;
}

/* 0x100eca08: max(floor% * max_pwm, v) - start point of a fade for a lit channel. */
static uint32_t yglc_mixer_floor_ticks(float floor_pct, const yglc_mixer_t *m, uint32_t v)
{
    uint32_t f = yglc_f2u((floor_pct / K_PCT) * (float)m->max_pwm);
    return (f < v) ? v : f;
}

/* ===================================================================================== */
/* Setters (0x100ebffa..0x100ec048) and constructors                                     */
/* ===================================================================================== */

static void set_max_pwm(yglc_mixer_t *m, uint32_t v)        { m->max_pwm = v; }
/* 0x100ebffe: `cmp r1,#100; it hs; movhs r1,#100; strb r1,[r0,#4]`.
 * Patch #9a replaces the clamp with `movs r1,#0` -> min_bright is always 0. */
static void set_min_bright_pct(yglc_mixer_t *m, uint32_t v) { m->min_bright_pct = (uint8_t)(v >= 100u ? 100u : v); }
static void set_trans_steps(yglc_mixer_t *m, uint32_t v)    { m->trans_steps = v; }
static void set_trans_curve(yglc_mixer_t *m, uint32_t v)    { m->trans_curve = (uint8_t)(v > 2u ? 1u : v); }
static void set_flag_0d(yglc_mixer_t *m, uint32_t v)        { m->flag_0d = (v != 0u); }
static void set_hook_enable(yglc_mixer_t *m, uint32_t v)    { m->hook_enable = (v != 0u); }
static void set_floor_remap(yglc_mixer_t *m, uint32_t v)    { m->floor_remap_enable = (v != 0u); }
static void set_flag_10(yglc_mixer_t *m, uint32_t v)        { m->flag_10 = (v != 0u); }
static void set_bright_curve(yglc_mixer_t *m, uint32_t v)   { m->bright_curve = (uint8_t)v; }

/* 0x100ec5b8: copies the 6-byte library version "4.7.0\0" (@0x10140520). */
static void get_version(yglc_mixer_t *m, char out_ver[6])
{
    static const char ver[6] = "4.7.0";
    (void)m;
    for (int i = 0; i < 6; i++) {
        out_ver[i] = ver[i];
    }
}

/* 0x100ec04a / 0x100ec130: no-op defaults for the RGB and direct entry points. */
static uint32_t misc_noop(yglc_mixer_t *m, ...) { (void)m; return 0u; }

/* 0x100ec5cc */
void yglc_mixer_base_init(yglc_mixer_t *m)
{
    m->set_max_pwm = set_max_pwm;
    m->set_min_bright_pct = set_min_bright_pct;
    m->set_trans_steps = set_trans_steps;
    m->set_trans_curve = set_trans_curve;
    m->set_flag_0d = set_flag_0d;
    m->set_hook_enable = set_hook_enable;
    m->set_floor_remap = set_floor_remap;
    m->set_flag_10 = set_flag_10;
    m->get_version = get_version;
    m->set_bright_curve = set_bright_curve;
    m->compute_target = NULL;   /* 0x100ec638 / 0x100ec63c: `bx lr` stubs in the base */
    m->interpolate = NULL;
    m->compute_rgb = misc_noop;  /* FUN_100ec05c in the original: 255-scaled 4ch math   */
    m->compute_direct = misc_noop;
    m->type = "G";
    m->bright_curve = YGLC_BRIGHT_CURVE_LINEAR;
}

static void yglc_mixer_common_reset(yglc_mixer_t *m)
{
    yglc_mixer_base_init(m);
    m->max_pwm = 0xffffu;
    m->min_bright_pct = 0u;
    m->trans_steps = 0u;
    m->trans_curve = YGLC_TRANS_CURVE_LINEAR;
    m->flag_0d = 0u;
    m->hook_enable = 0u;
    m->floor_remap_enable = 0u;
    m->calib = NULL;
    m->derate_gain = 1.0f;
}

/* 0x100ecbec -> 0x100ecbd8 -> 0x100ecb98: single-channel mixer ("nl"). */
static const yglc_single_calib_t *const g_nl_calib = (const yglc_single_calib_t *)0x20000d6cu;

yglc_mixer_t *yglc_mixer_new_single(void)
{
    yglc_mixer_t *m = pvPortMalloc(100);
    if (m != NULL) {
        yglc_mixer_common_reset(m);
        m->compute_target = yglc_mixer_single_compute;
        m->hook = yglc_mixer_thermal_derate;
        m->interpolate = yglc_mixer_single_interpolate;
        m->calib = g_nl_calib;
    }
    return m;
}

/* 0x100ecc5c -> 0x100ecc40 -> 0x100ecc00: two-channel CT mixer ("dl"). */
static const yglc_ct_calib_t *const g_dl_calib = (const yglc_ct_calib_t *)0x10140528u;

yglc_mixer_t *yglc_mixer_new_ct(void)
{
    yglc_mixer_t *m = pvPortMalloc(100);
    if (m != NULL) {
        yglc_mixer_common_reset(m);
        m->compute_target = yglc_mixer_ct_compute;
        m->interpolate = yglc_mixer_ct_interpolate;       /* 0x100ecac4 ...            */
        m->compute_direct = misc_noop;                    /* 0x100ec948, see below      */
        m->interpolate = yglc_mixer_dl_interpolate;       /* ... overridden: 0x100ec45c */
        m->calib = g_dl_calib;
    }
    return m;
}
/* Configuration done by yglc_init_settings (yglc_core.c ~765/795):
 *   dl: set_max_pwm(4000), set_floor_remap(1), set_hook_enable(0), get_version(settings+0x40)
 *   nl: set_max_pwm(4000), set_min_bright_pct(0), set_hook_enable(0), set_floor_remap(0)
 * Per transition the trans manager calls set_min_bright_pct(+0xd0 or +0xd4): dl 15/15,
 * nl 0/0, set_trans_curve(trans+0xcc/+0xcd: dl 2/2 = CUBIC_FROM, nl 1/0) and
 * set_trans_steps(n) (start_transition 0x100c07d4). Nobody calls set_bright_curve:
 * both mixers run LINEAR brightness.
 * NOTE: the 0x100ec948 "compute_direct" entry of the dl mixer takes a row-based packed
 * argument (mode byte, ct u16, value in per-mille u16) and produces
 * row.share/100 * max * value/1000 with the same floor remap; used for raw/flow output. */

/* ===================================================================================== */
/* Compute target                                                                         */
/* ===================================================================================== */

/* 0x100ec6a0: single channel. Returns 1 if bright was clamped to 100. */
uint8_t yglc_mixer_single_compute(yglc_mixer_t *m, uint32_t ct_k, uint8_t bright,
                                  uint32_t arg, uint32_t out[4])
{
    const yglc_single_calib_t *cal = (const yglc_single_calib_t *)m->calib;
    uint8_t clamped = bright > 100u;
    (void)ct_k;
    if (clamped) {
        bright = 100u;
    }
    float v = yglc_mixer_bright_scale(m, bright) * (float)m->max_pwm;
    if (m->hook_enable) {
        union { uint32_t u; float f; } a = { .u = arg };
        m->hook(a.f, m, &v);
    }
    if (m->floor_remap_enable) {
        yglc_mixer_floor_remap(cal->floor_pct, m, &v);
    }
    out[0] = yglc_f2u(v + K_ROUND);
    out[1] = out[2] = out[3] = 0u;
    return clamped;
}

/*
 * 0x100ec71c: two-channel CT mixer (daylight).
 *   share_w/share_c from the CT table: exact row for 2700..2999 K and for the last rows,
 *   quadratic Lagrange through 3 rows elsewhere; if a share falls below the channel
 *   floor that channel is dropped and the other one gets 100 %.
 *   out[0] = warm = share_w/100 * scale(bright) * max   (+ floor remap)
 *   out[1] = cool = share_c/100 * scale(bright) * max   (+ floor remap)
 * Return: 1 if bright or ct was clamped, 4 if no table.
 * NOTE: the constant 100.0 @0x100ec93c is both the divisor and the "100 %" replacement
 *       value (MEASURE build sets it to 1000.0 to scale the whole dl output by 0.1).
 */
uint8_t yglc_mixer_ct_compute(yglc_mixer_t *m, uint32_t ct_k, uint8_t bright,
                              uint32_t arg, uint32_t out[4])
{
    (void)arg;
    uint8_t clamped = bright > 100u;
    if (clamped) {
        bright = 100u;
    }
    ct_k &= 0xffffu;
    if (ct_k > YGLC_CT_MAX_K) {
        ct_k = YGLC_CT_MAX_K;
        clamped = 1u;
    } else if (ct_k < YGLC_CT_MIN_K) {
        ct_k = YGLC_CT_MIN_K;
        clamped = 1u;
    }
    float scale = yglc_mixer_bright_scale(m, bright);
    const yglc_ct_calib_t *t = (const yglc_ct_calib_t *)m->calib;
    if (t == NULL) {
        return 4u;
    }
    if (ct_k >= 0x1900u) {               /* >= 6400 K snaps to 6500 K */
        ct_k = YGLC_CT_MAX_K;
    }
    uint32_t r = yglc_ct_find_row(t, ct_k);
    float w, c;
    uint16_t rct = t->row[r].ct_k;
    if (rct == YGLC_CT_MIN_K || rct == YGLC_CT_MAX_K || t->row[r + 1].ct_k == YGLC_CT_MAX_K) {
        w = t->row[r].warm_pct;
        c = t->row[r].cool_pct;
    } else {
        while (t->row[r].ct_k > 0x1963u) {
            r = (r - 1u) & 0xffu;
        }
        if (t->row[r + 1].ct_k == YGLC_CT_MAX_K) {
            r = (r - 1u) & 0xffu;
        }
        if (t->row[r + 2].ct_k == YGLC_CT_MAX_K) {
            r = (r - 1u) & 0xffu;
        }
        float x = (float)ct_k;
        float x0 = t->row[r].ct_k, x1 = t->row[r + 1].ct_k, x2 = t->row[r + 2].ct_k;
        w = yglc_lagrange3(x, x0, t->row[r].warm_pct, x1, t->row[r + 1].warm_pct,
                           x2, t->row[r + 2].warm_pct);
        c = yglc_lagrange3(x, x0, t->row[r].cool_pct, x1, t->row[r + 1].cool_pct,
                           x2, t->row[r + 2].cool_pct);
        if (w < t->floor_warm_pct) {
            c = K_PCT;
            w = 0.0f;
        }
        if (c < t->floor_cool_pct) {
            c = 0.0f;
            w = K_PCT;
        }
    }
    float max = (float)m->max_pwm;
    float vw = (w / K_PCT) * scale * max;
    float vc = (c / K_PCT) * scale * max;
    if (m->floor_remap_enable) {
        yglc_mixer_floor_remap(t->floor_warm_pct, m, &vw);
        yglc_mixer_floor_remap(t->floor_cool_pct, m, &vc);
    }
    out[0] = yglc_f2u(vw + K_ROUND);
    out[1] = yglc_f2u(vc + K_ROUND);
    out[2] = out[3] = 0u;
    return clamped;
}

/*
 * 0x100ec204: thermal derating hook (only with hook_enable, which nobody sets).
 * temp >= 90 C: gain -= (temp*0.003 - 0.25), not below 0.6; temp < 75 C: gain += 0.02
 * up to 1.0. The target is multiplied by the gain and truncated. The gain resets to 1.0
 * when the target changes.
 */
void yglc_mixer_thermal_derate(float temp_c, yglc_mixer_t *m, float *value)
{
    static uint32_t last_target;           /* 0x2003a858 */
    uint32_t cur = yglc_f2u(*value);
    if (last_target != cur) {
        m->derate_gain = 1.0f;
        last_target = cur;
    }
    if (temp_c >= K_DERATE_HOT) {
        float g = m->derate_gain - (temp_c * K_DERATE_K - 0.25f);
        m->derate_gain = (g <= K_DERATE_MIN) ? K_DERATE_MIN : g;
    } else if (temp_c < K_DERATE_OK) {
        float g = m->derate_gain + K_DERATE_UP;
        m->derate_gain = (g < 1.0f) ? g : 1.0f;
    }
    *value = (float)yglc_f2u(*value * m->derate_gain);
}

/* ===================================================================================== */
/* Interpolation (one transition step)                                                    */
/* ===================================================================================== */

/* 0x100eca30: single channel. */
void yglc_mixer_single_interpolate(yglc_mixer_t *m, const uint32_t from[4],
                                   const uint32_t to[4], uint32_t step, uint32_t out[4])
{
    if (step == 0u || step >= m->trans_steps) {
        const uint32_t *src = (step == 0u) ? from : to;
        for (int i = 0; i < 4; i++) {
            out[i] = src[i];
        }
        return;
    }
    uint32_t f = from[0], t = to[0];
    if (m->floor_remap_enable && f != t) {
        float fl = ((const yglc_single_calib_t *)m->calib)->floor_pct;
        f = yglc_mixer_floor_ticks(fl, m, f);
        t = yglc_mixer_floor_ticks(fl, m, t);
    }
    out[0] = yglc_mixer_trans_ease(m, f, t, step, out[0]);
    out[1] = out[2] = out[3] = 0u;
}

/* 0x100ecac4: generic CT interpolation (installed, then overridden for dl). */
void yglc_mixer_ct_interpolate(yglc_mixer_t *m, const uint32_t from[4],
                               const uint32_t to[4], uint32_t step, uint32_t out[4])
{
    if (step == 0u || step >= m->trans_steps) {
        const uint32_t *src = (step == 0u) ? from : to;
        for (int i = 0; i < 4; i++) {
            out[i] = src[i];
        }
        return;
    }
    const yglc_ct_calib_t *t = (const yglc_ct_calib_t *)m->calib;
    uint32_t f0 = from[0], t0 = to[0], f1 = from[1], t1 = to[1];
    if (m->floor_remap_enable) {
        if (f0 != t0) {
            f0 = yglc_mixer_floor_ticks(t->floor_warm_pct, m, f0);
            t0 = yglc_mixer_floor_ticks(t->floor_warm_pct, m, t0);
        }
        if (f1 != t1) {
            f1 = yglc_mixer_floor_ticks(t->floor_cool_pct, m, f1);
            t1 = yglc_mixer_floor_ticks(t->floor_cool_pct, m, t1);
        }
    }
    out[0] = yglc_mixer_trans_ease(m, f0, t0, step, out[0]);
    out[1] = yglc_mixer_trans_ease(m, f1, t1, step, out[1]);
}

/*
 * 0x100ec45c: daylight interpolation. A channel that switches on starts from its floor
 * level (it never fades up from 0 through the dead zone); a channel that switches off
 * fades down to its floor and drops to 0 on the last step. If one channel turns on while
 * the other turns off (CT crossing a single-channel region), the endpoints are first
 * re-based to the 3/7 (on) or 4/7 (off) point of the fade so the colour crossover happens
 * mid-way.
 * NOTE: floors are swapped versus compute (ch0 uses +0x158, ch1 uses +0x154); harmless
 *       because both are equal.
 */
void yglc_mixer_dl_interpolate(yglc_mixer_t *m, const uint32_t from[4],
                               const uint32_t to[4], uint32_t step, uint32_t out[4])
{
    if (step == 0u || step >= m->trans_steps) {
        const uint32_t *src = (step == 0u) ? from : to;
        for (int i = 0; i < 4; i++) {
            out[i] = src[i];
        }
        return;
    }
    const yglc_ct_calib_t *t = (const yglc_ct_calib_t *)m->calib;
    float max = (float)m->max_pwm;
    uint32_t total = m->trans_steps;
    uint32_t f0 = from[0], f1 = from[1], t0 = to[0], t1 = to[1];
    uint32_t floor0 = yglc_f2u((t->floor_cool_pct / K_PCT) * max);   /* +0x158 */
    uint32_t floor1 = yglc_f2u((t->floor_warm_pct / K_PCT) * max);   /* +0x154 */
    enum { NONE = 0, CH0_OFF = 1, CH0_ON = 2 } ch0;

    if (f0 == 0u) {
        if (t0 == 0u) {
            ch0 = NONE;
        } else {
            ch0 = CH0_ON;
            f0 = floor0;
        }
    } else if (t0 == 0u) {
        ch0 = CH0_OFF;
        t0 = floor0;
    } else {
        ch0 = NONE;
    }

    uint32_t t1_eff = floor1;
    if (f1 == 0u) {
        if (t1 == 0u) {
            f1 = 0u;
            t1_eff = t1;
        } else {
            f1 = floor1;
            if (ch0 == CH0_ON) {
                uint32_t s = (total * 3u) / 7u;
                f0 = yglc_mixer_trans_ease(m, f0, t0, s, floor0);
                f1 = yglc_mixer_trans_ease(m, floor1, t1, s, floor1);
            }
            t1_eff = t1;
        }
    } else if (t1 == 0u) {
        if (ch0 == CH0_OFF) {
            uint32_t s = (total << 2) / 7u;
            t0 = yglc_mixer_trans_ease(m, f0, t0, s, floor0);
            t1_eff = yglc_mixer_trans_ease(m, f1, floor1, s, floor0);
        }
    } else {
        t1_eff = t1;
    }
    out[0] = yglc_mixer_trans_ease(m, f0, t0, step, out[0]);
    out[1] = yglc_mixer_trans_ease(m, f1, t1_eff, step, out[1]);
}

/* ===================================================================================== */
/* PWM output path                                                                        */
/* ===================================================================================== */

/* 0x100c0616 (called from FUN_100bacec with &cfg @0x200009a4). */
void yglc_pwm_init(yglc_pwm_config_t *cfg)
{
    hal_pwm_init(cfg->clock_src);
    for (int g = 0; g < 2; g++) {
        yglc_pwm_group_t *grp = &cfg->group[g];
        for (uint32_t i = 0; i < grp->pin_count; i++) {
            yglc_pwm_pin_t *p = &grp->pin[i];
            hal_gpt_delay_ms(2);
            hal_gpio_init(p->gpio);
            hal_pwm_set_frequency(p->pwm_channel, p->freq_hz, &p->period_ticks);
        }
    }
    hal_gpt_delay_ms(2);
    for (int g = 0; g < 2; g++) {
        yglc_pwm_group_t *grp = &cfg->group[g];
        hal_gpt_delay_ms(2);
        for (uint32_t i = 0; i < grp->pin_count; i++) {
            yglc_pwm_pin_t *p = &grp->pin[i];
            hal_pwm_set_duty_cycle(p->pwm_channel, p->inverted ? p->period_ticks : 0u);
            hal_pinmux_set_function(p->gpio, p->pinmux_func);
        }
    }
}

/* 0x100c0272: push every pin's duty to the hardware, start stopped channels. */
void yglc_pwm_group_output(yglc_pwm_group_t *g)
{
    for (uint32_t i = 0; i < g->pin_count; i++) {
        yglc_pwm_pin_t *p = &g->pin[i];
        uint32_t duty = p->inverted ? p->period_ticks - p->duty : p->duty;
        uint8_t running = 0u;
        hal_pwm_set_duty_cycle(p->pwm_channel & 0xffu, duty);
        if (hal_pwm_get_running_status(p->pwm_channel & 0xffu, &running) == 0 && !running) {
            hal_pwm_start(p->pwm_channel & 0xffu);
        }
    }
}

/*
 * 0x100c02bc: timer callback, one step of a transition (period = interval_x10 / 10 ms).
 * Normally out = mixer->interpolate(from, to, step). With smooth_restart (a new
 * transition replaced a running one) and 2 <= step < total, the jump between the old
 * output and the new curve is blended away over min(total - step, 16) steps.
 * Results go straight to pin.duty - there is no clamp anywhere after the mixer. Then the
 * step callback (+0x48) runs, and after the last step committed_bright = pending_bright,
 * smooth_restart = 0 and the done callback (+0x44, the effect manager) runs.
 */
void yglc_light_controller_continue_transition(void *timer, yglc_trans_view_t *tc)
{
    (void)timer;
    int32_t step = ++tc->step;
    int32_t total = tc->total_steps;
    if (step < total) {
        void *tmr = tc->timer;
        yglc_timer_del(tmr);
        if (yglc_timer_is_running(tmr)) {
            yglc_timer_del(tmr);
            log_printf("\n\n\n\nwarning %s: timer already running\n\n\n\n\n",
                       "yglc_light_controller_continue_transition");
        }
        *(void **)(tc->timer + 0x20) = tc;                                   /* +0x30 arg  */
        *(uint32_t *)(tc->timer + 0x0c) = tc->interval_x10 / 10u;           /* +0x1c      */
        *(uint32_t *)(tc->timer + 0x08) = 0u;                                /* +0x18      */
        *(uint32_t *)(tc->timer + 0x18) = 0u;                                /* +0x28      */
        *(void **)(tc->timer + 0x1c) = (void *)yglc_light_controller_continue_transition;
        yglc_timer_add(g_yglc_timer_service, tmr);
    }

    yglc_mixer_t *m = tc->mixer;
    step = tc->step;
    if (tc->smooth_restart == 0u || step < 2 || tc->total_steps <= step) {
        m->interpolate(m, tc->from, tc->to, (uint32_t)step, tc->out);
    } else {
        uint32_t old_out[4], prev_curve[4];
        for (int i = 0; i < 4; i++) {
            old_out[i] = tc->out[i];
            prev_curve[i] = tc->from[i];
            tc->out[i] = tc->from[i];
        }
        m->interpolate(m, tc->from, tc->to, (uint32_t)(step - 1), prev_curve);
        m->interpolate(m, tc->from, tc->to, (uint32_t)step, tc->out);
        int32_t n = total - step;           /* >= 1 here (step < total)             */
        n = (n > 15) ? 16 : n;
        for (int i = 0; i < 4; i++) {
            uint32_t cur = tc->out[i];
            uint32_t hi = (prev_curve[i] < old_out[i]) ? old_out[i] : prev_curve[i];
            if (hi < cur) {
                hi = cur;
            }
            g_yglc_trans_scratch = hi;
            if (prev_curve[i] < old_out[i]) {
                cur += ((uint32_t)(n - 1) * (old_out[i] - prev_curve[i])) / (uint32_t)n;
                tc->out[i] = (cur > hi) ? hi : cur;
            } else {
                uint32_t d = ((uint32_t)(n - 1) * (prev_curve[i] - old_out[i])) / (uint32_t)n;
                tc->out[i] = (d < cur) ? cur - d : 0u;
            }
        }
    }

    yglc_pwm_group_t *g = tc->pwm;
    g->pin[0].duty = tc->out[0];
    if (g->pin_count > 1u) {
        g->pin[1].duty = tc->out[1];
    }
    /* pin_count > 2 / > 3 write out[2] / out[3] (not present on ceiling3) */
    yglc_pwm_group_output(g);

    /* callbacks run AFTER the pins were written (the hybrid tail relies on this order) */
    if (tc->step_cb != NULL) {
        tc->step_cb(tc->ctx);
    }
    if (tc->total_steps <= tc->step) {
        tc->committed_bright = tc->pending_bright;
        tc->smooth_restart = 0u;
        if (tc->done_cb != NULL) {
            tc->done_cb(tc->ctx);
        }
    }
}
