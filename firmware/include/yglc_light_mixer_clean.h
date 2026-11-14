#pragma once
/*
 * Clean C11 reconstruction: Yeelight Ceiling3 (yeelink.light.ceiling3, 2.0.2_0048,
 * MT7697 Cortex-M4F) light MIXER + PWM OUTPUT path.
 *
 * Covers (segment 0, base 0x10071000):
 *   0x100ebf70..0x100ecc70  mixer library ("G" mixer, lib version "4.7.0" @0x10140520)
 *   0x100c0272              PWM group output            (yglc_pwm_group_output)
 *   0x100c02bc              transition step             (yglc_light_controller_continue_transition)
 *   0x100c0616              PWM init                    (yglc_pwm_init)
 *   0x1007343c..0x1007367c  MT7687 HAL PWM              (hal_pwm_*)
 *   0x10071700..0x100718f8  MT7687 PWM register driver  (pwm_set & friends)
 *
 * Full chain of a brightness change (all values are linear, nothing clamps below the
 * mixer):  bright/ct -> mixer->compute_target (0x50) -> per-step mixer->interpolate (0x54)
 *          -> continue_transition writes pin.duty -> hal_pwm_set_duty_cycle
 *          -> PWM register = (off_ticks << 16) | on_ticks, on_ticks = duty.
 * PWM: source clock 4 = 40 MHz, 10 kHz per pin -> period = 4000 ticks == mixer max_pwm.
 *
 * Hardware finding (measured 13.11.2026, 2700K, single warm channel, see LIGHT_MIXER.md):
 * the main LED driver does NOT dim below ~11 % duty: < ~5.0-5.5 % = off (turn-on needs
 * >= 5.6 %, small hysteresis), 5.6 %..~10.5 % = one constant minimum level (~ moonlight
 * at nl_br=100), visible dimming starts at ~10.5-11.2 %. Firmware path above is proven
 * linear, so this plateau is a property of the LED driver IC, not of the firmware.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------ */
/* PWM hardware configuration (RAM 0x200009a4, .data image @ flash 0x1014CE88)            */
/* ------------------------------------------------------------------------------------ */

#define YGLC_PWM_CLOCK_40MHZ   4u        /* hal_pwm_source_clock index; table @0x1010c37c:
                                          * {32700, 2M, 20M, 26M, 40M, 52M} Hz          */
#define YGLC_PWM_FREQ_HZ       10000u    /* every light pin                              */
#define YGLC_PWM_PERIOD_TICKS  4000u     /* 40 MHz / 10 kHz == mixer max_pwm             */

typedef struct yglc_pwm_pin {
    uint32_t gpio;          /* +0x00  GPIO number                                       */
    uint32_t pinmux_func;   /* +0x04  9 = PWM function                                  */
    uint32_t pwm_channel;   /* +0x08  hal_pwm channel                                    */
    uint32_t period_ticks;  /* +0x0c  filled by hal_pwm_set_frequency (= 4000)            */
    uint32_t reserved;      /* +0x10                                                     */
    uint32_t freq_hz;       /* +0x14  10000                                              */
    uint32_t duty;          /* +0x18  current on-time in ticks (written by transitions)   */
    uint32_t inverted;      /* +0x1c  1 -> hw duty = period - duty (unused: all 0)        */
} yglc_pwm_pin_t;

typedef struct yglc_pwm_group {
    uint32_t       pin_count;   /* +0x00                                                  */
    yglc_pwm_pin_t pin[2];      /* +0x04                                                  */
} yglc_pwm_group_t;

typedef struct yglc_pwm_config {
    uint8_t          clock_src; /* +0x00  YGLC_PWM_CLOCK_40MHZ                            */
    uint8_t          pad[3];
    yglc_pwm_group_t group[2];  /* +0x04  [0] "dl" (0x200009a8), [1] "nl" (0x200009ec)   */
} yglc_pwm_config_t;

/* Actual wiring of ceiling3 (from the .data image):
 *   dl group: GPIO31 -> PWM32 (warm, mixer out[0]), GPIO32 -> PWM33 (cool, mixer out[1])
 *   nl group: GPIO30 -> PWM31 (moonlight LEDs, mixer out[0])                              */

/* ------------------------------------------------------------------------------------ */
/* Calibration tables                                                                    */
/* ------------------------------------------------------------------------------------ */

typedef struct yglc_ct_calib_row {
    float    warm_pct;      /* +0x00  share of warm channel at this CT, 0..100           */
    float    cool_pct;      /* +0x04  share of cool channel                              */
    uint16_t reserved[4];   /* +0x08                                                     */
    uint16_t ct_k;          /* +0x10  CT of the row; > 9999 terminates the table         */
    uint16_t reserved2;     /* +0x12                                                     */
} yglc_ct_calib_row_t;

#define YGLC_CT_MIN_K  2700u   /* 0x0a8c */
#define YGLC_CT_MAX_K  6500u   /* 0x1964 */

/* dl table @0x10140528 (flash, const). Rows: 2700 3000 3200 3400 4000 4500 5000 5500 5800
 * 6000 6500, terminator 10000.                                                           */
typedef struct yglc_ct_calib {
    yglc_ct_calib_row_t row[17];   /* +0x000                                             */
    float floor_warm_pct;          /* +0x154  8.0 stock (patch #9b: 6.5, measure: 0.0)    */
    float floor_cool_pct;          /* +0x158  8.0 stock                                   */
} yglc_ct_calib_t;

/* nl "table" @0x20000d6c (RAM): only floor_pct is read; 5.0 (unused: remap disabled).    */
typedef struct yglc_single_calib {
    float floor_pct;
    float reserved[3];
} yglc_single_calib_t;

/* ------------------------------------------------------------------------------------ */
/* Mixer object (pvPortMalloc(100))                                                      */
/* ------------------------------------------------------------------------------------ */

typedef enum {
    YGLC_BRIGHT_CURVE_LINEAR    = 0,   /* default for both mixers                        */
    YGLC_BRIGHT_CURVE_QUADRATIC = 1,   /* implemented but never selected by the firmware  */
} yglc_bright_curve_t;

typedef enum {
    YGLC_TRANS_CURVE_LINEAR = 0,       /* nl when the bright value does not change         */
    YGLC_TRANS_CURVE_CUBIC_LOW_END = 1,/* cubic, slow near the darker end                 */
    YGLC_TRANS_CURVE_CUBIC_FROM = 2,   /* cubic from the start value                      */
} yglc_trans_curve_t;

struct yglc_mixer;
typedef struct yglc_mixer yglc_mixer_t;

typedef void     (*yglc_mixer_set_u32_fn)(yglc_mixer_t *m, uint32_t v);
typedef void     (*yglc_mixer_get_ver_fn)(yglc_mixer_t *m, char out_ver[6]);
/* returns "clamped" flag (bright > 100 or ct out of range); CT mixer returns 4 w/o table */
typedef uint8_t  (*yglc_mixer_compute_fn)(yglc_mixer_t *m, uint32_t ct_k, uint8_t bright,
                                          uint32_t arg, uint32_t out[4]);
typedef void     (*yglc_mixer_interp_fn)(yglc_mixer_t *m, const uint32_t from[4],
                                         const uint32_t to[4], uint32_t step, uint32_t out[4]);
typedef void     (*yglc_mixer_hook_fn)(float arg, yglc_mixer_t *m, float *value);
typedef uint32_t (*yglc_mixer_misc_fn)(yglc_mixer_t *m, ...);

struct yglc_mixer {
    uint32_t max_pwm;                /* +0x00  4000                                       */
    uint8_t  min_bright_pct;         /* +0x04  set per transition: dl 15 (#9a forces 0)   */
    uint8_t  pad05[3];
    uint32_t trans_steps;            /* +0x08  total steps of the running transition      */
    uint8_t  trans_curve;            /* +0x0c  yglc_trans_curve_t                         */
    uint8_t  flag_0d;                /* +0x0d  set_flag_0d (unused by ceiling3)           */
    uint8_t  hook_enable;            /* +0x0e  apply hook (+0x58) to target: thermal derate*/
    uint8_t  floor_remap_enable;     /* +0x0f  dl 1, nl 0                                 */
    uint8_t  flag_10;                /* +0x10                                             */
    uint8_t  pad11[3];
    float    derate_gain;            /* +0x14  1.0; thermal derating gain (0.6..1.0)      */
    uint8_t  bright_curve;           /* +0x18  yglc_bright_curve_t                        */
    uint8_t  pad19[3];
    const void *calib;               /* +0x1c  yglc_ct_calib_t* (dl) / yglc_single_calib_t* (nl) */
    uint32_t reserved20;             /* +0x20                                             */
    const char *type;                /* +0x24  "G"                                        */
    yglc_mixer_set_u32_fn set_max_pwm;          /* +0x28  0x100ebffa                     */
    yglc_mixer_set_u32_fn set_min_bright_pct;   /* +0x2c  0x100ebffe  (patch #9a site)   */
    yglc_mixer_set_u32_fn set_trans_steps;      /* +0x30  0x100ec008                     */
    yglc_mixer_set_u32_fn set_trans_curve;      /* +0x34  0x100ec00c                     */
    yglc_mixer_set_u32_fn set_flag_0d;          /* +0x38  0x100ec01a                     */
    yglc_mixer_set_u32_fn set_hook_enable;      /* +0x3c  0x100ec026                     */
    yglc_mixer_set_u32_fn set_floor_remap;      /* +0x40  0x100ec032                     */
    yglc_mixer_set_u32_fn set_flag_10;          /* +0x44  0x100ec03e                     */
    yglc_mixer_get_ver_fn get_version;          /* +0x48  0x100ec5b8  -> "4.7.0"         */
    yglc_mixer_set_u32_fn set_bright_curve;     /* +0x4c  0x100ec016                     */
    yglc_mixer_compute_fn compute_target;       /* +0x50  CT 0x100ec71c / single 0x100ec6a0 */
    yglc_mixer_interp_fn  interpolate;          /* +0x54  dl 0x100ec45c / CT 0x100ecac4 / single 0x100eca30 */
    yglc_mixer_hook_fn    hook;                 /* +0x58  single: thermal derate 0x100ec204 */
    yglc_mixer_misc_fn    compute_rgb;          /* +0x5c  0x100ec05c (generic 4ch, unused) */
    yglc_mixer_misc_fn    compute_direct;       /* +0x60  base 0x100ec130 / CT 0x100ec948 */
};

_Static_assert(offsetof(yglc_mixer_t, min_bright_pct) == 0x04, "mixer+0x04");
_Static_assert(offsetof(yglc_mixer_t, trans_steps) == 0x08, "mixer+0x08");
_Static_assert(offsetof(yglc_mixer_t, floor_remap_enable) == 0x0f, "mixer+0x0f");
_Static_assert(offsetof(yglc_mixer_t, derate_gain) == 0x14, "mixer+0x14");
_Static_assert(offsetof(yglc_mixer_t, bright_curve) == 0x18, "mixer+0x18");
_Static_assert(offsetof(yglc_ct_calib_t, floor_warm_pct) == 0x154, "calib+0x154");
_Static_assert(sizeof(yglc_ct_calib_row_t) == 0x14, "calib row");
_Static_assert(sizeof(yglc_pwm_pin_t) == 0x20, "pwm pin");
_Static_assert(sizeof(yglc_pwm_group_t) == 0x44, "pwm group");
#if UINTPTR_MAX == 0xffffffffu
_Static_assert(offsetof(yglc_mixer_t, set_max_pwm) == 0x28, "mixer vtable");
_Static_assert(offsetof(yglc_mixer_t, compute_target) == 0x50, "mixer+0x50");
_Static_assert(sizeof(yglc_mixer_t) == 100, "mixer size");
#endif

/* ------------------------------------------------------------------------------------ */
/* Transition controller (trans manager, pvPortCalloc(1, 0xe0)) - view of the fields     */
/* used by the output path. Full layout: yglc_light_unit_clean.h.                        */
/* ------------------------------------------------------------------------------------ */

typedef struct yglc_trans_view {
    uint8_t   opaque_00[0x10];
    uint8_t   timer[0x28];          /* +0x10  yglc_timer (period @+0x1c, cb @+0x2c, arg @+0x30) */
    int32_t   total_steps;          /* +0x38                                             */
    int32_t   step;                 /* +0x3c                                             */
    uint32_t  interval_x10;         /* +0x40  timer period = interval_x10 / 10           */
    void    (*done_cb)(void *ctx);  /* +0x44  called after the last step                 */
    void    (*step_cb)(void *ctx);  /* +0x48  called after every step (NULL in practice)  */
    void     *ctx;                  /* +0x4c  effect manager                              */
    uint8_t   opaque_50[0x38];
    uint32_t  from[4];              /* +0x88                                             */
    uint32_t  out[4];               /* +0x98  last value written to the pins             */
    uint32_t  to[4];                /* +0xa8                                             */
    yglc_pwm_group_t *pwm;          /* +0xb8                                             */
    yglc_mixer_t     *mixer;        /* +0xbc                                             */
    uint32_t  default_step_ms;      /* +0xc0                                             */
    uint32_t  committed_bright;     /* +0xc4  = pending_bright when a transition ends     */
    uint32_t  pending_bright;       /* +0xc8                                             */
    uint8_t   opaque_cc[0x0c];
    uint32_t  smooth_restart;       /* +0xd8  blend with previous output on retarget     */
    uint8_t   opaque_dc[4];
} yglc_trans_view_t;

#if UINTPTR_MAX == 0xffffffffu
_Static_assert(offsetof(yglc_trans_view_t, from) == 0x88, "trans+0x88");
_Static_assert(offsetof(yglc_trans_view_t, pwm) == 0xb8, "trans+0xb8");
_Static_assert(offsetof(yglc_trans_view_t, smooth_restart) == 0xd8, "trans+0xd8");
_Static_assert(offsetof(yglc_trans_view_t, done_cb) == 0x44, "trans+0x44");
_Static_assert(offsetof(yglc_trans_view_t, committed_bright) == 0xc4, "trans+0xc4");
_Static_assert(sizeof(yglc_trans_view_t) == 0xe0, "trans size");
#endif

/* ------------------------------------------------------------------------------------ */
/* API                                                                                    */
/* ------------------------------------------------------------------------------------ */

/* math helpers */
float    yglc_lagrange3(float x, float x0, float y0, float x1, float y1, float x2, float y2);
uint32_t yglc_ct_find_row(const yglc_ct_calib_t *t, uint32_t ct_k);
float    yglc_mixer_bright_scale(const yglc_mixer_t *m, uint32_t bright);
void     yglc_mixer_floor_remap(float floor_pct, const yglc_mixer_t *m, float *value);
uint32_t yglc_mixer_trans_ease(const yglc_mixer_t *m, uint32_t from, uint32_t to,
                               uint32_t step, uint32_t prev_out);

/* constructors */
void          yglc_mixer_base_init(yglc_mixer_t *m);       /* 0x100ec5cc */
yglc_mixer_t *yglc_mixer_new_single(void);                 /* 0x100ecbec (nl) */
yglc_mixer_t *yglc_mixer_new_ct(void);                     /* 0x100ecc5c (dl) */

/* compute / interpolate implementations */
uint8_t yglc_mixer_single_compute(yglc_mixer_t *m, uint32_t ct_k, uint8_t bright,
                                  uint32_t arg, uint32_t out[4]);
uint8_t yglc_mixer_ct_compute(yglc_mixer_t *m, uint32_t ct_k, uint8_t bright,
                              uint32_t arg, uint32_t out[4]);
void    yglc_mixer_single_interpolate(yglc_mixer_t *m, const uint32_t from[4],
                                      const uint32_t to[4], uint32_t step, uint32_t out[4]);
void    yglc_mixer_ct_interpolate(yglc_mixer_t *m, const uint32_t from[4],
                                  const uint32_t to[4], uint32_t step, uint32_t out[4]);
void    yglc_mixer_dl_interpolate(yglc_mixer_t *m, const uint32_t from[4],
                                  const uint32_t to[4], uint32_t step, uint32_t out[4]);
void    yglc_mixer_thermal_derate(float temp_c, yglc_mixer_t *m, float *value);

/* output path */
void yglc_pwm_init(yglc_pwm_config_t *cfg);                                  /* 0x100c0616 */
void yglc_pwm_group_output(yglc_pwm_group_t *g);                             /* 0x100c0272 */
void yglc_light_controller_continue_transition(void *timer, yglc_trans_view_t *tc); /* 0x100c02bc */

#ifdef __cplusplus
}
#endif
