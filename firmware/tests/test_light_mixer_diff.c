/*
 * Host side of the differential test: clean mixer (yglc_light_mixer_clean.c) on a fixed
 * input grid. tools/diff_light_mixer.py runs the ORIGINAL firmware functions on the same
 * grid in a Cortex-M4 emulator and compares line by line.
 *
 * Build (done by the python script):
 *   gcc -std=c11 -O1 -ffp-contract=off -DHOST_TEST=1 test_light_mixer_diff.c
 *       ../clean_src/yglc_light_mixer_clean.c -o build/test_light_mixer_diff.exe
 * Run:  test_light_mixer_diff.exe <segment-0-flash-00071000.bin>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/yglc_light_mixer_clean.h"

/* ---- stubs for the firmware symbols the clean module references ---- */
int hal_pwm_init(uint8_t c) { (void)c; return 0; }
int hal_pwm_set_frequency(uint32_t ch, uint32_t f, uint32_t *t) { (void)ch; *t = 40000000u / f; return 0; }
int hal_pwm_set_duty_cycle(uint32_t ch, uint32_t d) { (void)ch; (void)d; return 0; }
int hal_pwm_start(uint32_t ch) { (void)ch; return 0; }
int hal_pwm_get_running_status(uint32_t ch, uint8_t *r) { (void)ch; *r = 1; return 0; }
int hal_gpio_init(uint32_t g) { (void)g; return 0; }
int hal_pinmux_set_function(uint32_t g, uint32_t f) { (void)g; (void)f; return 0; }
int hal_gpt_delay_ms(uint32_t ms) { (void)ms; return 0; }
void *pvPortMalloc(size_t n) { return calloc(1, n); }
int yglc_timer_add(void *s, void *t) { (void)s; (void)t; return 0; }
int yglc_timer_del(void *t) { (void)t; return 0; }
int yglc_timer_is_running(void *t) { (void)t; return 0; }
int log_printf(const char *fmt, ...) { (void)fmt; return 0; }
void *g_yglc_timer_service;
uint32_t g_yglc_trans_scratch;

#define BASE 0x10071000u
static uint8_t *seg;

static void setup(yglc_mixer_t *m, const void *calib, int ct)
{
    memset(m, 0, sizeof(*m));
    yglc_mixer_base_init(m);
    m->max_pwm = 4000u;
    m->derate_gain = 1.0f;
    m->calib = calib;
    if (ct) {
        m->compute_target = yglc_mixer_ct_compute;
        m->interpolate = yglc_mixer_dl_interpolate;
        m->floor_remap_enable = 1u;
    } else {
        m->compute_target = yglc_mixer_single_compute;
        m->interpolate = yglc_mixer_single_interpolate;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        return 3;
    }
    seg = malloc(0x100000);
    size_t n = fread(seg, 1, 0x100000, f);
    fclose(f);
    if (n < 0xdf000) {
        return 4;
    }
    yglc_ct_calib_t cal;
    memcpy(&cal, seg + (0x10140528u - BASE), sizeof(cal));
    static const float floors[] = { 8.0f, 6.5f, 0.0f, 10.5f };
    static const uint8_t mins[] = { 0, 1, 15 };
    yglc_mixer_t m;

    /* 1. CT compute: floor x min x curve x ct x bright */
    for (unsigned fi = 0; fi < 4; fi++) {
        cal.floor_warm_pct = cal.floor_cool_pct = floors[fi];
        for (unsigned mi = 0; mi < 3; mi++) {
            for (unsigned cv = 0; cv < 2; cv++) {
                for (unsigned ct = 2600; ct <= 6600; ct += 50) {
                    for (unsigned b = 0; b <= 101; b += (b < 20 ? 1 : 7)) {
                        uint32_t out[4] = { 0 };
                        setup(&m, &cal, 1);
                        m.min_bright_pct = mins[mi];
                        m.bright_curve = (uint8_t)cv;
                        uint8_t r = yglc_mixer_ct_compute(&m, ct, (uint8_t)b, 0, out);
                        printf("C %u %u %u %u %u %u %u %u\n", fi, mi, cv, ct, b, r, out[0], out[1]);
                    }
                }
            }
        }
    }
    /* 2. single compute (nl): min x bright */
    yglc_single_calib_t nl = { 5.0f, { 0, 0, 0 } };
    for (unsigned mi = 0; mi < 3; mi++) {
        for (unsigned b = 0; b <= 101; b++) {
            uint32_t out[4] = { 0 };
            setup(&m, &nl, 0);
            m.min_bright_pct = mins[mi];
            uint8_t r = yglc_mixer_single_compute(&m, 0, (uint8_t)b, 0, out);
            printf("S %u %u %u %u\n", mi, b, r, out[0]);
        }
    }
    /* 3. ease: curve x (from,to) x step, prev = from */
    static const uint32_t pts[][2] = { { 0, 4000 }, { 4000, 0 }, { 320, 1200 }, { 1200, 320 }, { 77, 78 }, { 500, 500 } };
    for (unsigned cv = 0; cv < 3; cv++) {
        for (unsigned p = 0; p < 6; p++) {
            for (unsigned s = 0; s <= 30; s++) {
                setup(&m, &cal, 1);
                m.trans_steps = 30;
                m.trans_curve = (uint8_t)cv;
                uint32_t r = yglc_mixer_trans_ease(&m, pts[p][0], pts[p][1], s, pts[p][0]);
                printf("E %u %u %u %u\n", cv, p, s, r);
            }
        }
    }
    /* 4. dl interpolate: (from,to) pairs incl. channels switching on/off */
    static const uint32_t pairs[][4] = {
        { 0, 0, 2000, 1500 }, { 2000, 1500, 0, 0 }, { 0, 900, 900, 0 }, { 900, 0, 0, 900 },
        { 320, 320, 4000, 4000 }, { 0, 2000, 3000, 0 }, { 3000, 0, 0, 2000 }, { 400, 0, 1600, 0 } };
    for (unsigned fi = 0; fi < 3; fi++) {
        cal.floor_warm_pct = cal.floor_cool_pct = floors[fi];
        for (unsigned p = 0; p < 8; p++) {
            for (unsigned s = 0; s <= 21; s++) {
                setup(&m, &cal, 1);
                m.trans_steps = 21;
                uint32_t from[4] = { pairs[p][0], pairs[p][1], 0, 0 };
                uint32_t to[4] = { pairs[p][2], pairs[p][3], 0, 0 };
                uint32_t out[4] = { from[0], from[1], 0, 0 };
                yglc_mixer_dl_interpolate(&m, from, to, s, out);
                printf("I %u %u %u %u %u\n", fi, p, s, out[0], out[1]);
            }
        }
    }
    return 0;
}
