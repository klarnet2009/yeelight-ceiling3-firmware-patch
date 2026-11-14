/*
 * Clean C11 reconstruction of the Yeelight Ceiling3 (yeelink.light.ceiling3, 2.0.2_0048,
 * MT7697 Cortex-M4F) light unit / light channel / effect manager / trans manager.
 *
 * Source of truth: Thumb-2 disassembly of extracted/segment-0-flash-00071000.bin
 * (0x100c06a0..0x100c26c8, + 0x100be560, 0x100be6bc, 0x100c021c..0x100c0272,
 * 0x100c0578..0x100c0616). Ghidra output for this range is unreliable (missing functions,
 * merged tail calls, wrong arities) - every function below was re-read from the asm.
 *
 * ===================================================================================
 *  Ответы (подробно: decomp/LIGHT_UNIT.md)
 * ===================================================================================
 * 1. Структуры: yglc_light_unit_t 0x164, yglc_light_channel_t 0x1b4, yglc_effect_manager_t
 *    0x20 (+ стек эффектов yglc_light_effect_t 0x3c, узлы потока 0x24),
 *    yglc_trans_manager_t 0xe0 - см. заголовок, все смещения под _Static_assert.
 *
 * 2. dl <-> nl. Индекс unit->active (+0x10) меняют только:
 *      yglc_light_unit_select_channel  0x100c1db8 (по маске режима: CT -> dl, mode 7 -> nl),
 *      yglc_light_unit_activate        0x100c1cdc (disable_nl, nl_type 0: active = 0),
 *      yglc_light_unit_nl_set_bright   0x100c2008 (enable_nl, nl_type 0: active = 1),
 *      yglc_light_unit_power_on        0x100c23b0 (дневной путь: active = 0),
 *      on_flow_step / on_flow_end      0x100c19bc / 0x100c1bc6 (миграция потока).
 *    Старый канал всегда гасится эффектом OFF (power_off, длительность = dur вызывающего,
 *    в nl-путях фиксированные 300 мс), новый зажигается своим STATIC эффектом с dur
 *    вызывающего - т.е. кроссфейд двух независимых trans manager'ов. Яркости каналов
 *    независимы: dl->bright ("bright") и nl->bright ("nl_br") не трогают друг друга.
 *    Отчётные поля пересчитывает callback yglc_light_unit_on_channel_state (0x100c1ad0)
 *    на каждый commit/старт эффекта: nl_type 0: active==1 -> nl_active=1, active_mode=1,
 *    nl_br_effective = nl->bright; иначе nl_active=0, active_mode=0, nl_br_effective=0,
 *    nl_status=0 (!); active_bright = bright активного канала; unit->off = off активного.
 *    Переход: steps = ceil(clamp(dur,300,3600000) / 10 мс), период таймера 1 тик (10 мс).
 *
 * 3. bright -> mixer: unit_set_bright 0x100c1d4a -> channel_set_bright 0x100c16f4 ->
 *    channel_apply 0x100c1434 (effect STATIC, tuple{ct,bright,dur}) -> effect_start
 *    0x100c1014 -> resume 0x100c0f24 -> next_trans 0x100c0910 -> [BL @0x100c09ce] ->
 *    start_transition 0x100c07d4: mixer+0x34(curve), +0x2c(min_bright d0/d4),
 *    +0x50 compute_target(mixer, ct, bright, 1.0f, tm->target_pwm) (bright==0 -> target=0),
 *    +0x30(steps) -> таймер -> 0x100c02bc -> +0x54 interpolate. Второй (и последний)
 *    путь к +0x50 - retarget 0x100c06d4 (смена яркости во время потока).
 *    Гибрид: перехват единственного BL @0x100c09ce (см. yglc_hybrid_start_transition внизу).
 *
 * 4. Power on: запрос 1 -> yglc_light_api_power_on 0x100c2574 -> set_power_on_mode
 *    0x100c243c (mode 0) -> если unit->off: yglc_light_unit_power_on 0x100c23b0:
 *    дневной свет, если nl_type==3 или (!is_night_time() && nl_status != 1), иначе
 *    ночник (enable_nl, nl_status не меняется) + авто-выключение по cron, если ночь и
 *    settings->night_autooff_min. На загрузке FUN_100bac8c ставит unit->off=1 и шлёт
 *    тот же запрос 1 (mode 0, 500 мс); включать ли вообще - init_power_opt (+0x100).
 *
 * Bugs found (marked BUG below): NULL deref after failed calloc in new_trans_manager;
 * strtol(NULL) in flow parser; flow effect leak + read of address 0x10 as a duration in
 * on_flow_end (recover after channel migration); no empty-stack checks after unlink.
 */

#include "../include/yglc_light_mixer_clean.h"   /* yglc_mixer_t, yglc_pwm_group_t, output path */
#include "../include/yglc_light_unit_clean.h"

/* ---- libc / FreeRTOS / yglc runtime (resolved by address at link time) ---- */
void  *memset(void *dst, int c, size_t n);                 /* 0x101075b8 */
void  *memcpy(void *dst, const void *src, size_t n);       /* 0x10107570 */
size_t strlen(const char *s);                              /* 0x1010854e */
char  *strncpy(char *dst, const char *src, size_t n);      /* 0x101085d2 */
char  *strcpy(char *dst, const char *src);                 /* 0x10108162 */
int    snprintf(char *buf, size_t size, const char *fmt, ...); /* 0x10107f90 */
long   strtol(const char *s, char **end, int base);        /* 0x101094ac */
char  *yglc_strtok_r(char *s, const char *delim, char **save); /* 0x101093a2 */
void  *pvPortCalloc(size_t nmemb, size_t size);            /* 0x100c9130 */
void   vPortFree(void *p);                                 /* 0x100c9138 */
int    log_printf(const char *fmt, ...);                   /* 0x10107a34 */
void   arch_dbg_log_begin(const char *color);              /* 0x100a8100 */
void   arch_dbg_log_end(const char *color);                /* 0x100a8180 */

int    yglc_timer_add(void *service, void *timer);         /* 0x100c354c */
int    yglc_timer_del(void *timer);                        /* 0x100c35a0 */
int    yglc_timer_is_running(void *timer);                 /* 0x100c358e */
int    yglc_post_request_async(uint32_t id, const void *data, uint32_t len,
                               uint32_t a, uint32_t b);    /* 0x100bdbc0 */

/* yglc_pwm_group_output (0x100c0272) and yglc_light_controller_continue_transition
 * (0x100c02bc) come from yglc_light_mixer_clean.h. */

/* other yglc_core.c routines referenced by this module */
int      FUN_100c0158(uint32_t type);   /* cron: cancel jobs of a type (0 = "off" job). ASSUMPTION */
void     FUN_100be34c(uint32_t off);    /* power-state hook (indicator/relay?).        ASSUMPTION */
void     FUN_100c30b8(uint32_t force);  /* yglc_report_props: no-op if settings->report_mute */
int32_t  FUN_100f715a(void *miio);      /* miio: timezone offset, seconds              */
uint32_t FUN_100a8650(void);            /* UTC seconds                                 */
void     FUN_100a8fd4(const uint32_t *t, int32_t tm_out[9]); /* gmtime_r: sec,min,hour,... */

#define yglc_cron_cancel_type    FUN_100c0158
#define yglc_power_state_hook    FUN_100be34c
#define yglc_report_props        FUN_100c30b8
#define miio_get_tz_offset       FUN_100f715a
#define arch_utc_seconds         FUN_100a8650
#define arch_gmtime_r            FUN_100a8fd4

/* ---- SRAM globals ---- */
#define LU_LOG_LEVEL        (*(volatile const int32_t *)0x20004a2cu)   /* g_log_level        */
#define LU_MIIO_HANDLE      (*(void *const volatile *)0x20004b24u)     /* g_miio_net.handle  */
#define LU_SETTINGS         (*(yglc_lu_settings_t *const volatile *)0x20006908u)
#define LU_NTP_SYNCED       (*(volatile const uint32_t *)0x20007fb8u)  /* FUN_100be504/510   */
#define LU_TIMER_SERVICE    (*(void *const volatile *)0x20008020u)     /* yglc timer service */
#define LU_LAST_IS_NL       (*(volatile uint8_t *)0x20008048u)         /* channel_apply      */
#define LU_RGB_NEW_IDX      (*(volatile int32_t *)0x20008050u)         /* adjust_rgb_new     */
#define LU_RGB_IDX          (*(volatile int32_t *)0x20008054u)         /* adjust_rgb         */
#define LU_EMPTY_FLOW_STR   ((const char *)0x101171a0u)                /* ""                 */

#define LU_ERR(...)                                                         \
    do {                                                                    \
        if (LU_LOG_LEVEL < 4) {                                             \
            arch_dbg_log_begin("\x1b[0;31m");                               \
            log_printf(__VA_ARGS__);                                        \
            arch_dbg_log_end("\x1b[0m");                                    \
        }                                                                   \
    } while (0)

#define LU_INFO(...)                                                        \
    do {                                                                    \
        if (LU_LOG_LEVEL < 2) {                                             \
            arch_dbg_log_begin("\x1b[0;32m");                               \
            log_printf(__VA_ARGS__);                                        \
            arch_dbg_log_end("\x1b[0m");                                    \
        }                                                                   \
    } while (0)

/* ---- mixer: typed vtable from yglc_light_mixer_clean.h ---- */

/* +0x5c is declared variadic there (generic 4ch, unused by ceiling3); the original passes
 * the four channel values as 32-bit floats in r1..r3 + stack (soft-float ABI), which a
 * variadic call would promote to double -> read the slot with the exact original type. */
typedef void (*lu_mixer_raw_fn)(yglc_mixer_t *m, float c2, float c1, float c0, float c3,
                                uint8_t arg, uint32_t out[4]);
_Static_assert(offsetof(yglc_mixer_t, compute_rgb) == 0x5c, "mixer+0x5c");

static lu_mixer_raw_fn lu_mixer_raw_slot(const yglc_mixer_t *m)
{
    return *(lu_mixer_raw_fn const *)(const void *)&m->compute_rgb;
}

/* compute_target receives the raw tuple words: ct|pad<<16, bright (|has_ct<<8 in r2, the
 * mixer reads the low byte), scale bits (always 1.0f). */
static void lu_mixer_compute(yglc_mixer_t *m, const yglc_trans_tuple_t *t, uint32_t out[4])
{
    uint32_t scale_bits;
    memcpy(&scale_bits, &t->scale, sizeof scale_bits);
    (void)m->compute_target(m, (uint32_t)t->ct | ((uint32_t)t->_pad_02 << 16), t->bright,
                            scale_bits, out);
}

/* 0x100c1590..0x100c15c4 / 0x100c02bc: out[i] -> pin[i].duty for i < pin_count. The original
 * would also write +0x5c/+0x7c for pin_count > 2 (past yglc_pwm_group_t); ceiling3 has <= 2. */
static void lu_pwm_group_load(yglc_pwm_group_t *g, const uint32_t v[4])
{
    g->pin[0].duty = v[0];
    if (g->pin_count > 1u) {
        g->pin[1].duty = v[1];
    }
}

/* ---- list helpers ---- */
#define LU_CONTAINER(ptr, type, member) \
    ((type *)(void *)((uint8_t *)(void *)(ptr) - offsetof(type, member)))

static bool lu_list_empty(const yglc_lu_list_t *head) { return head->next == head; }

static void lu_list_unlink_init(yglc_lu_list_t *n)
{
    n->next->prev = n->prev;
    n->prev->next = n->next;
    n->next = n;
    n->prev = n;
}

static yglc_flow_node_t *lu_node_of(yglc_lu_list_t *link)
{
    return LU_CONTAINER(link, yglc_flow_node_t, link);
}

/* Original: link pointer compared with 0x1c, i.e. container_of(link) == NULL. */
static bool lu_link_is_null_node(const yglc_lu_list_t *link)
{
    return (uintptr_t)link == offsetof(yglc_flow_node_t, link);
}

static yglc_light_effect_t *lu_effect_of_stack(yglc_lu_list_t *link)
{
    return LU_CONTAINER(link, yglc_light_effect_t, stack);
}

static yglc_light_channel_t *lu_active_channel(const yglc_light_unit_t *u)
{
    return (u->channel_count > 1) ? u->channel[u->active] : u->channel[0];
}

/* Next flow node, exactly as next_trans/on_flow_step pick it (NULL link -> NULL). */
static yglc_lu_list_t *lu_flow_peek_next(yglc_effect_flow_t *f)
{
    yglc_lu_list_t *next = f->nodes.next;
    if (next == &f->nodes) {
        return NULL;
    }
    if (f->cur != NULL && f->cur->next != &f->nodes) {
        next = f->cur->next;
    }
    return next;
}

/* Callback tables used by the channel layer. */
static void lu_fill_channel_cbs(yglc_effect_cbs_t *cbs, yglc_light_channel_t *ch, bool flow)
{
    memset(cbs, 0, sizeof *cbs);
    cbs->on_start        = yglc_light_channel_on_effect_start;
    cbs->before_trans    = yglc_channel_before_trans;
    cbs->on_finish       = yglc_channel_on_effect_finish;
    cbs->flow_step_check = flow ? yglc_channel_flow_step_check : NULL;
    cbs->ctx = ch;
}

/* Request 1/2 payload (FUN_100bdcfa / FUN_100bdd0e). */
typedef struct {
    uint32_t mask;      /* +0x00 */
    uint8_t  has_dur;   /* +0x04 */
    uint8_t  _pad[3];
    uint32_t dur_ms;    /* +0x08 */
    uint32_t mode;      /* +0x0c */
    uint32_t _unk_10;   /* +0x10 */
} lu_power_req_t;
_Static_assert(sizeof(lu_power_req_t) == 0x14, "power req");

/* =====================================================================================
 * Helpers outside the range
 * ===================================================================================== */

/* 0x100c0578: step through a sorted table. */
int32_t yglc_table_step(uint32_t action, int32_t v, const int32_t *t, uint32_t len)
{
    int32_t i;
    int32_t last = (int32_t)len - 1;

    switch (action) {
    case YGLC_ADJ_TABLE_UP:
        for (i = 0; i < last; i++) {
            if (v >= t[i] && v < t[i + 1]) {
                return t[i + 1];
            }
        }
        return t[i];                       /* t[len-1] (t[0] if len <= 1) */
    case YGLC_ADJ_TABLE_DOWN:
        for (i = 0; i < last; i++) {
            if (v > t[i] && v <= t[i + 1]) {
                return t[i];
            }
        }
        return t[0];
    case 4:
    case 5:                                /* circle */
        for (i = 0; i < last; i++) {
            if (v >= t[i] && v < t[i + 1]) {
                return t[i + 1];
            }
        }
        return t[0];
    default:
        return t[0];
    }
}

/* 0x100c0238: |dB| + |dG| + |dR|. */
uint32_t yglc_rgb_distance(uint32_t a, uint32_t b)
{
    int32_t d0 = (int32_t)(a & 0xffu) - (int32_t)(b & 0xffu);
    int32_t d1 = ((int32_t)(a & 0xff00u) - (int32_t)(b & 0xff00u)) >> 8;
    int32_t d2 = ((int32_t)(a & 0xff0000u) - (int32_t)(b & 0xff0000u)) >> 16;
    return (uint32_t)((d0 < 0 ? -d0 : d0) + (d1 < 0 ? -d1 : d1) + (d2 < 0 ? -d2 : d2));
}

/* 0x100c05e4: tuple the effect is showing (flow: current node; NULL node -> (tuple*)4). */
const yglc_trans_tuple_t *yglc_effect_current_tuple(const yglc_light_effect_t *e)
{
    if (e->type == YGLC_EFFECT_FLOW && e->u.flow.cur != NULL) {
        return &lu_node_of(e->u.flow.cur)->tuple;
    }
    if (e->type == YGLC_EFFECT_FLOW) {
        return (const yglc_trans_tuple_t *)(uintptr_t)4u;   /* BUG: no NULL check */
    }
    return &e->u.tuple;
}

/* 0x100c05f4: effect cb +0x30 (last transition finished). */
void yglc_channel_on_effect_finish(void *ctx, yglc_light_effect_t *e)
{
    yglc_light_channel_t *ch = (yglc_light_channel_t *)ctx;
    if (e->type != YGLC_EFFECT_FLOW) {
        return;
    }
    ch->flow_active = 0;
    if (ch->on_flow_end != NULL) {
        ch->on_flow_end(ch->owner, ch, &e->u.flow);
    }
}

/* 0x100c021c: effect cb +0x34 (flow may migrate to another channel). */
int yglc_channel_flow_step_check(void *ctx, yglc_light_effect_t *e)
{
    yglc_light_channel_t *ch = (yglc_light_channel_t *)ctx;
    if (ch->on_flow_step == NULL) {
        return 0;
    }
    return ch->on_flow_step(ch->owner, ch, &e->u.flow);
}

/* =====================================================================================
 * Trans manager
 * ===================================================================================== */

/* 0x100c06a0 */
uint32_t yglc_trans_tuple_steps(yglc_trans_tuple_t *t)
{
    if (t->duration_ms <= 300u) {
        t->duration_ms = 300u;
    }
    if (t->duration_ms > 3600000u) {
        t->duration_ms = 3600000u;
    }
    if (t->step_ms < 10u) {
        t->step_ms = 10u;
    }
    return (t->duration_ms + t->step_ms - 1u) / t->step_ms;
}

/* 0x100c06d4 (not in functions.csv): change the target of a running transition. */
void yglc_trans_manager_retarget(yglc_trans_manager_t *tm, yglc_trans_tuple_t *t, int keep_from)
{
    tm->retarget = 1u;
    if (keep_from) {
        tm->prev.bright = t->bright;                 /* re-scale the segment start too */
        lu_mixer_compute(tm->mixer, &tm->prev, tm->from_pwm);
    }
    lu_mixer_compute(tm->mixer, t, tm->target_pwm);

    t->step_ms = tm->default_step_ms;
    int32_t total = (int32_t)yglc_trans_tuple_steps(t);
    int32_t old_total = tm->total_steps;
    tm->total_steps = total;
    int32_t cur = (total * tm->cur_step) / old_total;    /* sdiv: /0 -> 0 on Cortex-M */
    tm->cur_step = cur;

    int32_t step = (int32_t)tm->default_step_ms;
    int32_t min_left = (step + 199) / step;              /* >= 200 ms left */
    int32_t left = total - cur;
    if (left < min_left) {
        if (left == 0) {
            left = 1;
        }
        tm->total_steps = (min_left * total) / left;
        tm->cur_step = (min_left * cur) / left;
    }
    tm->mixer->set_trans_steps(tm->mixer, (uint32_t)tm->total_steps);
}

/* 0x100c07d4 "yglc_trans_manager_start_transition" (only caller: BL @0x100c09ce). */
int yglc_trans_manager_start_transition(yglc_trans_manager_t *tm, yglc_trans_tuple_t *t,
                                        const yglc_trans_cbs_t *cbs)
{
    yglc_mixer_t *m;

    t->scale = 1.0f;
    tm->retarget = 0u;
    tm->prev = tm->cur;
    tm->cur = *t;

    m = tm->mixer;
    if (tm->committed_bright == t->bright) {
        m->set_trans_curve(m, tm->curve_same_bright);   /* + r2 = committed (ignored) */
    } else {
        tm->pending_bright = t->bright;
        m->set_trans_curve(m, tm->curve_new_bright);
    }

    memcpy(tm->from_pwm, tm->out_pwm, sizeof tm->from_pwm);
    m->set_min_bright_pct(m, (uint8_t)(t->is_nl != 0u ? tm->min_bright_nl : tm->min_bright));

    if (t->bright == 0u) {
        tm->pending_bright = 0u;
        memset(tm->target_pwm, 0, sizeof tm->target_pwm);
    } else {
        lu_mixer_compute(tm->mixer, t, tm->target_pwm);
    }
    if (t->is_nl != 0u) {
        tm->target_pwm[3] = 0u;
    }

    uint32_t steps = yglc_trans_tuple_steps(t);
    tm->mixer->set_trans_steps(tm->mixer, steps);
    tm->total_steps = (int32_t)steps;
    tm->cur_step = 0;
    tm->step_ms = tm->default_step_ms;

    yglc_timer_del(&tm->timer);
    if (cbs != NULL) {
        memcpy(&tm->cbs, cbs, sizeof tm->cbs);
    } else {
        memset(&tm->cbs, 0, sizeof tm->cbs);
    }
    if (yglc_timer_is_running(&tm->timer)) {
        yglc_timer_del(&tm->timer);
        log_printf("\n\n\n\nwarning %s: timer already running\n\n\n\n\n",
                   "yglc_trans_manager_start_transition");
    }
    tm->timer.cb = yglc_light_controller_continue_transition;
    tm->timer.arg = tm;
    tm->timer.period = 0u;           /* first step on the next tick */
    tm->timer.elapsed = 0u;
    tm->timer.repeat = 0u;
    yglc_timer_add(LU_TIMER_SERVICE, &tm->timer);
    return 0;
}

/* 0x100c0dcc */
yglc_trans_manager_t *yglc_new_trans_manager(const char *name, struct yglc_mixer *mixer,
                                             struct yglc_pwm_group *pwm,
                                             uint8_t curve_new_bright, uint8_t curve_same_bright,
                                             uint32_t default_step_ms, uint32_t min_bright,
                                             uint32_t min_bright_nl)
{
    yglc_trans_manager_t *tm = (yglc_trans_manager_t *)pvPortCalloc(1, sizeof *tm);
    if (tm == NULL) {
        LU_ERR("[E] failed to malloc for trans manager\r\n (%s,%d)", "yglc_new_trans_manager", 0x371);
        /* BUG: original falls through and writes through NULL */
    }
    strncpy(tm->name, name, 15);
    tm->default_step_ms = default_step_ms;
    tm->curve_same_bright = curve_same_bright;
    tm->min_bright = min_bright;
    tm->mixer = mixer;
    tm->pwm = pwm;
    tm->curve_new_bright = curve_new_bright;
    tm->min_bright_nl = min_bright_nl;
    return tm;
}

/* =====================================================================================
 * Effects
 * ===================================================================================== */

/* 0x100c0760 (not in functions.csv; Ghidra glued it to sync_update_to_trans). */
void yglc_effect_manager_retarget(yglc_effect_manager_t *mgr, int keep_from)
{
    yglc_light_effect_t *e = mgr->top;
    yglc_trans_tuple_t *t;

    switch (e->type) {
    case YGLC_EFFECT_STATIC:
    case YGLC_EFFECT_OFF:
    case YGLC_EFFECT_STATIC_8:
        t = &e->u.tuple;
        break;
    case YGLC_EFFECT_FLOW:
        t = &lu_node_of(e->u.flow.cur)->tuple;     /* BUG: cur may be NULL */
        break;
    default:
        LU_ERR("[E] %s ERR: type=%d\r\n (%s,%d)", "yglc_light_effect_sync_update_to_trans",
               e->type, "yglc_light_effect_sync_update_to_trans", 0x3f1);
        t = NULL;
        break;
    }
    yglc_trans_manager_retarget(mgr->trans, t, keep_from);
}

/* 0x100c0910 */
void yglc_light_effect_next_trans(yglc_effect_manager_t *mgr, int started)
{
    yglc_trans_cbs_t tcbs = { yglc_effect_on_trans_done, NULL, mgr };
    yglc_light_effect_t *e = mgr->top;
    yglc_trans_tuple_t *t = NULL;
    void (*cb)(void *, yglc_light_effect_t *);

    switch (e->type) {
    case YGLC_EFFECT_STATIC:
    case YGLC_EFFECT_OFF:
    case YGLC_EFFECT_STATIC_8:
        if (started != 1) {
            goto finished;
        }
        t = &e->u.tuple;
        break;
    case YGLC_EFFECT_FLOW: {
        yglc_effect_flow_t *f = &e->u.flow;
        if (f->allow_switch != 0u && e->cbs.flow_step_check != NULL &&
            e->cbs.flow_step_check(e->cbs.ctx, e) != 0) {
            return;                                /* flow moved to the other channel */
        }
        e = mgr->top;
        f = &e->u.flow;
        if (f->count > 0 && f->iter >= f->count) {
            goto finished;
        }
        yglc_lu_list_t *next = lu_flow_peek_next(f);
        if (next == NULL) {
            goto finished;
        }
        f->cur = next;
        f->iter++;
        if (lu_link_is_null_node(next)) {
            goto finished;
        }
        t = &lu_node_of(next)->tuple;
        break;
    }
    default:
        LU_ERR("[E] %s ERR: type=%d\r\n (%s,%d)", "yglc_light_effect_next_trans",
               e->type, "yglc_light_effect_next_trans", 0x3bc);
        goto finished;
    }

    t->step_ms = mgr->trans->default_step_ms;
    e = mgr->top;
    if (e->cbs.before_trans != NULL) {
        (void)e->cbs.before_trans(e->cbs.ctx, e);
    }
    yglc_trans_manager_start_transition(mgr->trans, t, &tcbs);
    if (started != 1) {
        return;
    }
    e = mgr->top;
    cb = e->cbs.on_start;
    goto call;

finished:
    e = mgr->top;
    cb = e->cbs.on_finish;
call:
    if (cb != NULL) {
        cb(e->cbs.ctx, e);
    }
}

/* 0x100c0a04: trans manager done-callback (+0x44); tail-calls the no-op 0x100bdcf8. */
void yglc_effect_on_trans_done(void *mgr)
{
    yglc_light_effect_next_trans((yglc_effect_manager_t *)mgr, 0);
}

/* 0x100c0a14: append a copy of *n to the flow. */
int yglc_effect_flow_chain_add(yglc_effect_flow_t *f, const yglc_flow_node_t *n)
{
    yglc_flow_node_t *node = (yglc_flow_node_t *)pvPortCalloc(1, sizeof *node);
    if (node == NULL) {
        LU_ERR("[E] %s, No mem\r\n (%s,%d)", "yglc_effect_flow_chain_add",
               "yglc_effect_flow_chain_add", 0x21a);
        return 5;
    }
    memcpy(node, n, sizeof *node);
    yglc_lu_list_t *tail = f->nodes.prev;
    f->nodes.prev = &node->link;
    node->link.next = &f->nodes;
    node->link.prev = tail;
    tail->next = &node->link;
    return 0;
}

/* 0x100c0a80 */
void yglc_effect_flow_chain_free(yglc_effect_flow_t *f)
{
    yglc_lu_list_t *link = f->nodes.next;
    while (link != &f->nodes) {
        yglc_lu_list_t *nxt = link->next;
        link->prev->next = link->next;           /* original order: next->prev, prev->next */
        link->next->prev = link->prev;
        link->next = NULL;
        link->prev = NULL;
        vPortFree(lu_node_of(link));
        link = nxt;
    }
    f->nodes.next = &f->nodes;
    f->nodes.prev = &f->nodes;
}

/* 0x100c0abc: add delta to every node duration, round to 10 ms, clamp to [1 s, 10 s]. */
void yglc_effect_flow_add_duration(yglc_effect_flow_t *f, int32_t delta_ms)
{
    for (yglc_lu_list_t *l = f->nodes.next; l != &f->nodes; l = l->next) {
        yglc_flow_node_t *n = lu_node_of(l);
        int32_t d = (((int32_t)n->tuple.duration_ms + delta_ms) / 10) * 10;
        if (d > 10000) {
            d = 10000;
        } else if (d < 1000) {
            d = 1000;
        }
        n->tuple.duration_ms = (uint32_t)d;
    }
}

/* 0x100c0af8 */
int yglc_effect_flow_set_bright(yglc_effect_flow_t *f, uint8_t bright)
{
    for (yglc_lu_list_t *l = f->nodes.next; l != &f->nodes; l = l->next) {
        lu_node_of(l)->tuple.bright = bright;
    }
    return 0;
}

/* 0x100c0b14 */
yglc_light_effect_t *yglc_light_effect_create_new(uint32_t type)
{
    yglc_light_effect_t *e = (yglc_light_effect_t *)pvPortCalloc(1, sizeof *e);
    if (e == NULL) {
        return NULL;
    }
    e->stack.next = &e->stack;
    e->stack.prev = &e->stack;
    e->type = (uint8_t)type;
    switch (type) {
    case YGLC_EFFECT_STATIC:
    case YGLC_EFFECT_OFF:
    case YGLC_EFFECT_STATIC_8:
        break;
    case YGLC_EFFECT_FLOW:
        e->u.flow.nodes.next = &e->u.flow.nodes;
        e->u.flow.nodes.prev = &e->u.flow.nodes;
        break;
    default:
        LU_ERR("[E] %s ERR: type=%d\r\n (%s,%d)", "yglc_light_effect_create_new", (int)type,
               "yglc_light_effect_create_new", 0x2c2);
        break;
    }
    return e;
}

/* 0x100c0b8c (called with &mgr->top) */
void yglc_light_effect_free(yglc_light_effect_t **pe)
{
    yglc_light_effect_t *e = *pe;
    if (e == NULL) {
        return;
    }
    switch (e->type) {
    case YGLC_EFFECT_STATIC:
    case YGLC_EFFECT_OFF:
    case YGLC_EFFECT_STATIC_8:
        break;
    case YGLC_EFFECT_FLOW:
        yglc_effect_flow_chain_free(&e->u.flow);
        break;
    default:
        LU_ERR("[E] %s ERR: type=%d\r\n (%s,%d)", "yglc_light_effect_free", e->type,
               "yglc_light_effect_free", 0x2dc);
        break;
    }
    vPortFree(*pe);
    *pe = NULL;
}

/* 0x100c0bfc: "count,action,dur,mode,value,bright[,dur,mode,value,bright...]". */
int yglc_light_effect_parse_flow_string(const char *s, yglc_light_effect_t **out)
{
    yglc_flow_node_t tmp;      /* reused between tuples: mode 3/8 inherit ct/has_ct */
    char buf[256];
    char *save;
    char *tok;
    char *br_tok;

    memset(&tmp, 0, sizeof tmp);
    if (s == NULL || strlen(s) == 0u) {
        return 1;
    }
    strncpy(buf, s, sizeof buf);
    buf[255] = '\0';
    *out = yglc_light_effect_create_new(YGLC_EFFECT_FLOW);
    if (*out == NULL) {
        return 5;
    }
    tok = yglc_strtok_r(buf, ",", &save);
    if (tok == NULL) {
        goto fail;
    }
    (*out)->u.flow.count = (int32_t)strtol(tok, NULL, 10);
    tok = yglc_strtok_r(NULL, ",", &save);
    if (tok == NULL) {
        goto fail;
    }
    (*out)->u.flow.action = (uint8_t)strtol(tok, NULL, 10);

    for (;;) {
        tok = yglc_strtok_r(NULL, ",", &save);
        if (tok == NULL) {
            return 0;
        }
        tmp.tuple.duration_ms = (uint32_t)strtol(tok, NULL, 10);

        tok = yglc_strtok_r(NULL, ",", &save);
        if (tok == NULL) {
            LU_ERR("[E] parse : incomplete tuple, missing cf_type\r\n (%s,%d)",
                   "yglc_light_effect_parse_flow_string", 0x31e);
            goto fail;
        }
        tmp.mode = (uint32_t)strtol(tok, NULL, 10);

        tok = yglc_strtok_r(NULL, ",", &save);
        if (tok == NULL && tmp.mode != 3u && (tmp.mode - 7u) > 1u) {
            LU_ERR("[E] parse : incomplete tuple, missing color info\r\n (%s,%d)",
                   "yglc_light_effect_parse_flow_string", 0x328);
            goto fail;
        }
        long value = strtol(tok, NULL, 10);      /* BUG: tok may be NULL for modes 3/7/8 */

        br_tok = yglc_strtok_r(NULL, ",", &save);
        if (br_tok == NULL && ((tmp.mode - 1u) <= 2u || tmp.mode == 8u)) {
            LU_ERR("[E] parse : incomplete tuple, missing bright\r\n (%s,%d)",
                   "yglc_light_effect_parse_flow_string", 0x334);
            goto fail;
        }
        long bright = strtol(br_tok, NULL, 10);  /* BUG: NULL for modes 4..7 at the end */

        switch (tmp.mode) {
        case YGLC_CMODE_CT:
        case YGLC_CMODE_CT_BR:
            tmp.tuple.has_ct = 1u;
            tmp.tuple.ct = (uint16_t)value;
            /* fall through */
        case YGLC_CMODE_HSV:
        case YGLC_CMODE_NL_FLOW:
            tmp.tuple.bright = (uint8_t)bright;
            break;
        case YGLC_CMODE_SLEEP:
            break;
        default:                                 /* 1 (RGB), 4, 6 rejected */
            LU_ERR("[E] unsupported color mode %d\r\n (%s,%d)", (int)tmp.mode,
                   "yglc_light_effect_parse_flow_string", 0x359);
            goto fail;
        }
        if (yglc_effect_flow_chain_add(&(*out)->u.flow, &tmp) != 0) {
            goto fail;
        }
        if (br_tok == NULL) {
            return 0;
        }
    }

fail:
    yglc_light_effect_free(out);
    return 1;
}

/* 0x100c0e50 */
yglc_effect_manager_t *yglc_new_effect_manager(const char *name, yglc_trans_manager_t *tm)
{
    yglc_effect_manager_t *m = (yglc_effect_manager_t *)pvPortCalloc(1, sizeof *m);
    if (m == NULL) {
        LU_ERR("[E] failed to malloc for effect manager\r\n (%s,%d)", "yglc_new_effect_manager", 0x386);
        return NULL;
    }
    m->stack.next = &m->stack;
    m->stack.prev = &m->stack;
    m->top = NULL;
    m->trans = tm;
    strncpy(m->name, name, 15);
    return m;
}

/* 0x100c0eb4: new bright for every node of the running flow, retarget keeping the start. */
void yglc_light_effect_sync_update_to_trans(yglc_effect_manager_t *mgr, uint8_t bright)
{
    yglc_effect_flow_set_bright(&mgr->top->u.flow, bright);
    yglc_effect_manager_retarget(mgr, 1);
}

/* 0x100c0ecc */
void yglc_effect_flow_adjust_speed(yglc_effect_manager_t *mgr, int32_t delta_ms)
{
    if (mgr->top->type != YGLC_EFFECT_FLOW) {
        return;
    }
    yglc_effect_flow_add_duration(&mgr->top->u.flow, delta_ms);
    yglc_effect_manager_retarget(mgr, 0);
}

/* 0x100c0eec */
void yglc_effect_manager_stop(yglc_effect_manager_t *mgr)
{
    yglc_timer_del(&mgr->trans->timer);
}

/* 0x100c0ef4 */
void yglc_light_effect_push(yglc_effect_manager_t *mgr, yglc_light_effect_t *e, int start)
{
    yglc_effect_manager_stop(mgr);
    yglc_lu_list_t *first = mgr->stack.next;
    first->prev = &e->stack;
    e->stack.next = first;
    e->stack.prev = &mgr->stack;
    mgr->stack.next = &e->stack;
    mgr->top = e;
    if (start) {
        yglc_light_effect_next_trans(mgr, 1);
    }
}

/* 0x100c0f24: restart the top effect (optionally with a new duration for STATIC/STATIC_8). */
void yglc_light_effect_resume(yglc_effect_manager_t *mgr, int32_t duration_ms)
{
    yglc_effect_manager_stop(mgr);
    if (duration_ms >= 0 && (mgr->top->type & (YGLC_EFFECT_STATIC | YGLC_EFFECT_STATIC_8)) != 0u) {
        mgr->top->u.tuple.duration_ms = (uint32_t)duration_ms;
    }
    yglc_light_effect_next_trans(mgr, 1);
}

/* 0x100c0f4c */
void yglc_light_effect_pop(yglc_effect_manager_t *mgr, int resume)
{
    if (lu_list_empty(&mgr->stack)) {
        LU_ERR("[E] !!! light effect stack empty !!! \r\n (%s,%d)", "yglc_light_effect_pop", 0x427);
        return;
    }
    yglc_effect_manager_stop(mgr);
    yglc_light_effect_t *e = mgr->top;
    e->stack.next->prev = e->stack.prev;
    e->stack.prev->next = e->stack.next;
    e->stack.next = NULL;
    e->stack.prev = NULL;
    yglc_light_effect_free(&mgr->top);
    mgr->top = NULL;
    if (lu_list_empty(&mgr->stack)) {
        return;
    }
    mgr->top = lu_effect_of_stack(mgr->stack.next);
    if (resume == 1) {
        yglc_light_effect_resume(mgr, -1);
    }
}

/* 0x100c0fcc */
void yglc_light_effect_pop_types(yglc_effect_manager_t *mgr, uint8_t type_mask)
{
    while (!lu_list_empty(&mgr->stack) && (type_mask & mgr->top->type) != 0u) {
        yglc_light_effect_pop(mgr, 0);
    }
}

/* 0x100c0fee: drop finite flows (count != 0); infinite flows stay under OFF/STATIC_8. */
void yglc_light_effect_pop_finite_flows(yglc_effect_manager_t *mgr)
{
    while (!lu_list_empty(&mgr->stack) && mgr->top->type == YGLC_EFFECT_FLOW &&
           mgr->top->u.flow.count != 0) {
        yglc_light_effect_pop(mgr, 0);
    }
}

/* 0x100c1014 */
void yglc_light_effect_start(yglc_effect_manager_t *mgr, yglc_light_effect_t *e,
                             const yglc_effect_cbs_t *cbs)
{
    switch (e->type) {
    case YGLC_EFFECT_STATIC:      /* replaces everything, incl. infinite flows */
        yglc_light_effect_pop_types(mgr, 0x0f);
        break;
    case YGLC_EFFECT_FLOW:
        yglc_light_effect_pop_finite_flows(mgr);
        yglc_light_effect_pop_types(mgr, YGLC_EFFECT_STATIC_8);
        break;
    case YGLC_EFFECT_OFF:
        yglc_light_effect_pop_finite_flows(mgr);
        yglc_light_effect_pop_types(mgr, YGLC_EFFECT_OFF | YGLC_EFFECT_STATIC_8);
        break;
    case YGLC_EFFECT_STATIC_8:
        yglc_light_effect_pop_types(mgr, YGLC_EFFECT_FLOW | YGLC_EFFECT_OFF | YGLC_EFFECT_STATIC_8);
        break;
    default:
        LU_ERR("[E] ERR: type=%d\r\n (%s,%d)", e->type, "yglc_light_effect_start", 0x47e);
        break;
    }
    yglc_light_effect_push(mgr, e, 0);
    if (cbs != NULL) {
        memcpy(&mgr->top->cbs, cbs, sizeof mgr->top->cbs);
    } else {
        memset(&mgr->top->cbs, 0, sizeof mgr->top->cbs);
    }
    yglc_light_effect_resume(mgr, -1);
}

/* =====================================================================================
 * Light channel
 * ===================================================================================== */

/* 0x100c10b4 */
yglc_light_channel_t *yglc_new_light_channel(const char *name, yglc_effect_manager_t *m,
                                             uint32_t mode_mask)
{
    yglc_light_channel_t *ch = (yglc_light_channel_t *)pvPortCalloc(1, sizeof *ch);
    if (ch == NULL) {
        LU_ERR("[E] failed to malloc for light channel\r\n (%s,%d)", "yglc_new_light_channel", 0x4b2);
        return NULL;
    }
    ch->mode_mask = mode_mask;
    ch->effects = m;
    ch->enabled = 1u;
    strncpy(ch->name, name, 15);
    return ch;
}

/* 0x100c1120: resolve "relative" flow tuples against the channel state. */
int yglc_channel_fill_flow_tuple(yglc_light_channel_t *ch, yglc_flow_node_t *n)
{
    yglc_trans_tuple_t *t = &n->tuple;

    switch (n->mode) {
    case YGLC_CMODE_RGB:
        break;
    case YGLC_CMODE_HSV:
    case YGLC_CMODE_NL_FLOW:
        if (ch->flow_last_mode == YGLC_CMODE_CT || ch->flow_last_mode == YGLC_CMODE_SLEEP) {
            t->has_ct = 1u;
            t->ct = (uint16_t)ch->flow_last_ct;
        } else {
            LU_ERR("[E] %s %d ERR: mode=%d\r\n (%s,%d)", "yglc_channel_fill_flow_tuple", 0x4e3,
                   (int)ch->flow_last_mode, "yglc_channel_fill_flow_tuple", 0x4e3);
        }
        break;
    case YGLC_CMODE_CT_BR:
        t->has_ct = 1u;
        t->bright = (uint8_t)ch->bright;
        /* fall through */
    case YGLC_CMODE_CT:
        t->has_ct = 1u;
        if (t->ct == 0u) {
            t->ct = (uint16_t)ch->ct;
        } else {
            int32_t v = (int32_t)t->ct;
            int32_t r;
            if (v < (int32_t)ch->min_ct) {
                r = (int32_t)ch->min_ct;
            } else {
                r = (v < (int32_t)ch->max_ct) ? v : (int32_t)ch->max_ct;
            }
            t->ct = (uint16_t)r;
        }
        break;
    case YGLC_CMODE_FLOW:
        t->has_ct = 0u;
        t->bright = (uint8_t)ch->bright;
        break;
    case YGLC_CMODE_SLEEP:         /* hold: copy ct/bright/scale of the running transition */
        memcpy(t, &ch->effects->trans->cur, 12);
        break;
    default:
        LU_ERR("[E] %s %d ERR: type=%d\r\n (%s,%d)", "yglc_channel_fill_flow_tuple", 0x500,
               (int)n->mode, "yglc_channel_fill_flow_tuple", 0x500);
        break;
    }
    if ((uint32_t)t->bright - 1u > 99u) {
        t->bright = (uint8_t)ch->bright;
    }
    return 0;
}

/* 0x100c1204: effect cb +0x2c. Returns the node bright (non-flow: r0 = ctx, unused). */
uint32_t yglc_channel_before_trans(void *ctx, yglc_light_effect_t *e)
{
    yglc_light_channel_t *ch = (yglc_light_channel_t *)ctx;
    if (e->type != YGLC_EFFECT_FLOW) {
        return 0u;
    }
    yglc_flow_node_t *n = (e->u.flow.cur != NULL) ? lu_node_of(e->u.flow.cur) : NULL;
    yglc_channel_fill_flow_tuple(ch, n);         /* BUG: n == NULL not handled */
    return n->tuple.bright;
}

/* 0x100c1224: rebuild ch->flow_str from the running infinite flow. */
int yglc_light_channel_update_flow_string(yglc_light_channel_t *ch)
{
    char buf[256];
    yglc_light_effect_t *e = ch->effects->top;
    uint32_t value = 0u;     /* r7: starts at count (== 0), kept across nodes */
    int n;

    if (e->type != YGLC_EFFECT_FLOW || e->u.flow.count != 0) {
        return 0;
    }
    n = snprintf(buf, sizeof buf, "%d,%d", 0, e->u.flow.action);
    for (yglc_lu_list_t *l = e->u.flow.nodes.next; l != &e->u.flow.nodes; l = l->next) {
        yglc_flow_node_t *node = lu_node_of(l);
        if (n > 0xe2) {
            LU_ERR("[E] flow param buffer out of space\r\n (%s,%d)",
                   "yglc_light_channel_update_flow_string", 0x52a);
            return 6;
        }
        yglc_channel_fill_flow_tuple(ch, node);
        uint32_t mode = node->mode;
        if (mode == YGLC_CMODE_CT || mode == YGLC_CMODE_CT_BR ||
            (mode == YGLC_CMODE_HSV && node->tuple.has_ct == 1u)) {
            value = node->tuple.ct;
            mode = YGLC_CMODE_CT;
        }
        n += snprintf(buf + n, (size_t)(256 - n), ",%d,%d,%u,%d", (int)node->tuple.duration_ms,
                      (int)mode, (unsigned)value, (int)node->tuple.bright);
    }
    strcpy(ch->flow_str, buf);
    return 0;
}

/* 0x100c12f8: take a finished/started tuple into the channel state. */
void yglc_light_channel_commit_tuple(yglc_light_channel_t *ch, const yglc_trans_tuple_t *t, int commit)
{
    ch->last_bright = t->bright;
    if (ch->effects->top->type != YGLC_EFFECT_OFF) {
        ch->is_nl = (yglc_effect_current_tuple(ch->effects->top)->is_nl != 0u) ? 1u : 0u;
    }
    if (t->bright != 0u) {
        ch->flow_last_mode = YGLC_CMODE_CT;
        ch->flow_last_ct = t->ct;
        if (!commit) {
            goto notify;
        }
        if (ch->is_nl != 1u) {
            ch->ct = t->ct;
            ch->color_mode = YGLC_CMODE_CT;
        }
    } else if (!commit) {
        goto notify;
    }
    if (t->bright != 0u) {
        ch->off = 0u;
        ch->bright = t->bright;
    } else {
        ch->off = 1u;
    }
notify:
    if (ch->on_state_change != NULL) {
        ch->on_state_change(ch->owner, ch);
    }
}

/* 0x100c136c: effect cb +0x28 (first transition of an effect started). */
void yglc_light_channel_on_effect_start(void *ctx, yglc_light_effect_t *e)
{
    yglc_light_channel_t *ch = (yglc_light_channel_t *)ctx;
    int commit = 0;

    switch (e->type) {
    case YGLC_EFFECT_STATIC:
        commit = 1;
        ch->off = 0u;
        ch->flow_active = 0u;
        break;
    case YGLC_EFFECT_FLOW:
        ch->off = 0u;
        ch->flow_active = 1u;
        break;
    case YGLC_EFFECT_OFF:
        ch->off = 1u;
        break;
    case YGLC_EFFECT_STATIC_8:
        ch->off = 0u;
        ch->flow_active = 0u;
        break;
    default:
        LU_ERR("[E] %s ERR: type=%d\r\n (%s,%d)", "yglc_light_channel_on_effect_start", e->type,
               "yglc_light_channel_on_effect_start", 0x5d9);
        break;
    }
    if (e->type != YGLC_EFFECT_STATIC_8) {
        yglc_light_channel_commit_tuple(ch, yglc_effect_current_tuple(e), commit);
        /* + yglc_effect_current_tuple(e) passed to FUN_100bffde: a "return 0" stub */
    }
    if (ch->on_state_change != NULL) {
        ch->on_state_change(ch->owner, ch);
    }
}

/* 0x100c1418 (not in functions.csv) */
void yglc_light_channel_flow_adjust_speed(yglc_light_channel_t *ch, int32_t delta_ms)
{
    if (ch->flow_active == 0u) {
        return;
    }
    yglc_effect_flow_adjust_speed(ch->effects, delta_ms);
    yglc_light_channel_update_flow_string(ch);
}

/* 0x100c1434: the single funnel for every static color change of a channel. */
int yglc_light_channel_apply(yglc_light_channel_t *ch, yglc_color_req_t *req,
                             uint32_t duration_ms, int as_type8)
{
    yglc_effect_cbs_t cbs;
    lu_fill_channel_cbs(&cbs, ch, false);

    yglc_light_effect_t *e = yglc_light_effect_create_new(as_type8 ? YGLC_EFFECT_STATIC_8
                                                                    : YGLC_EFFECT_STATIC);
    if (e == NULL) {
        return 5;
    }
    ch->is_nl = (req->mode == YGLC_CMODE_SLEEP) ? 1u : 0u;
    e->u.tuple.is_nl = ch->is_nl;

    /* left nightlight mode (global, not per channel)? */
    int left_nl = 0;
    if (LU_LAST_IS_NL != ch->is_nl) {
        LU_LAST_IS_NL = ch->is_nl;
        left_nl = (ch->is_nl == 0u);
    }
    yglc_light_unit_t *main_unit = LU_SETTINGS->unit;   /* always the "main" unit */
    if (ch->restore_saved != 0u) {
        ch->restore_saved = 0u;
        if (left_nl) {
            *req = main_unit->saved_req;
        }
    }
    if (!(req->bright == 1u && (req->value & 0xffffffu) == 0xff9800u) && ch->is_nl == 0u) {
        main_unit->saved_req = *req;
    }

    e->u.tuple.has_ct = 1u;
    e->u.tuple.ct = (uint16_t)req->value;      /* RGB / 0xff9800 truncated to 16 bits */
    e->u.tuple.duration_ms = duration_ms;
    e->u.tuple.bright = (uint8_t)req->bright;
    yglc_light_effect_start(ch->effects, e, &cbs);
    return 0;
}

/* 0x100c1520 (not in functions.csv): direct PWM write, no transition. */
int yglc_light_channel_set_raw(yglc_light_channel_t *ch, const uint8_t rgbw[4], uint8_t arg)
{
    yglc_trans_manager_t *tm = ch->effects->trans;
    yglc_pwm_group_t *grp = tm->pwm;

    yglc_effect_manager_stop(ch->effects);
    lu_mixer_raw_slot(tm->mixer)(tm->mixer, (float)rgbw[2], (float)rgbw[1], (float)rgbw[0],
                                 (float)rgbw[3], arg, tm->out_pwm);
    lu_pwm_group_load(grp, tm->out_pwm);
    yglc_pwm_group_output(tm->pwm);
    return 0;
}

/* 0x100c15d4 (not in functions.csv) */
int yglc_light_channel_set_rgb(yglc_light_channel_t *ch, const uint32_t *rgb, int32_t bright,
                               uint32_t duration_ms)
{
    yglc_color_req_t req;
    memset(&req, 0, sizeof req);
    req.mode = YGLC_CMODE_RGB;
    req.bright = (bright < 0) ? ch->bright : (uint32_t)bright;
    if (rgb != NULL && *rgb - 1u <= 0xfffffeu) {
        req.value = *rgb & 0xffffffu;
    } else {
        req.value = ch->rgb & 0xffffffu;
    }
    return yglc_light_channel_apply(ch, &req, duration_ms, 0);
}

/* 0x100c1640 (not in functions.csv): nl_type 1 moonlight (orange, bright 1). */
int yglc_light_channel_set_nl_rgb(yglc_light_channel_t *ch, uint32_t duration_ms)
{
    yglc_color_req_t req = { YGLC_CMODE_SLEEP, 0xff9800u, 0u /* uninit stack */, 1u };
    return yglc_light_channel_apply(ch, &req, duration_ms, 0);
}

/* 0x100c1664 (not in functions.csv): nl_type 2 moonlight (min CT, bright 1). */
int yglc_light_channel_set_nl_ct(yglc_light_channel_t *ch, uint32_t duration_ms)
{
    yglc_color_req_t req = { YGLC_CMODE_SLEEP, ch->min_ct, 0u /* uninit stack */, 1u };
    return yglc_light_channel_apply(ch, &req, duration_ms, 0);
}

/* 0x100c1682 (not in functions.csv) */
int yglc_light_channel_set_hsv(yglc_light_channel_t *ch, const int32_t hs[2], int32_t bright,
                               uint32_t duration_ms)
{
    yglc_color_req_t req;
    req.mode = YGLC_CMODE_HSV;
    req.bright = (bright < 0) ? ch->bright : (uint32_t)bright;
    req.value = (hs != NULL && hs[0] >= 0) ? (uint32_t)hs[0] : ch->hue;
    req.aux = (hs != NULL && hs[1] >= 0) ? (uint32_t)hs[1] : ch->sat;
    return yglc_light_channel_apply(ch, &req, duration_ms, 0);
}

/* 0x100c16bc (not in functions.csv) */
int yglc_light_channel_set_ct(yglc_light_channel_t *ch, int32_t ct, int32_t bright,
                              uint32_t duration_ms)
{
    yglc_color_req_t req;
    req.mode = YGLC_CMODE_CT;
    if (ct <= 0) {
        req.value = ch->ct;
    } else if (ct < (int32_t)ch->min_ct) {
        req.value = ch->min_ct;
    } else {
        req.value = (ct < (int32_t)ch->max_ct) ? (uint32_t)ct : ch->max_ct;
    }
    req.aux = 0u;                                   /* uninitialised in the original */
    req.bright = (bright < 0) ? ch->bright : (uint32_t)bright;
    return yglc_light_channel_apply(ch, &req, duration_ms, 0);
}

/* 0x100c16f4 */
int yglc_light_channel_set_bright(yglc_light_channel_t *ch, int32_t bright, uint32_t duration_ms,
                                  int as_type8)
{
    yglc_light_effect_t *top = ch->effects->top;

    if (top != NULL && top->type == YGLC_EFFECT_FLOW) {
        yglc_light_effect_sync_update_to_trans(ch->effects, (uint8_t)bright);
        yglc_light_channel_update_flow_string(ch);
        ch->bright = (uint32_t)bright;
        yglc_report_props(0);
        return 0;
    }

    yglc_color_req_t req;
    req.mode = ch->color_mode;
    req.bright = (bright > 0) ? (uint32_t)bright : ch->bright;
    req.aux = 0u;                                   /* uninitialised in the original */
    if (req.mode != YGLC_CMODE_CT) {
        LU_ERR("[E] %s: color mode %d not suppored \r\n (%s,%d)", ch->name, (int)ch->color_mode,
               "yglc_light_channel_set_bright", 0x7a7);
        return 1;
    }
    req.value = ch->ct;
    return yglc_light_channel_apply(ch, &req, duration_ms, as_type8);
}

/* 0x100c1780: re-apply committed bright/ct, then re-start flow / re-darken if needed. */
int yglc_light_channel_restore(yglc_light_channel_t *ch, uint32_t duration_ms)
{
    uint32_t was_off = ch->off;
    uint32_t was_flow = ch->flow_active;

    int r = yglc_light_channel_set_bright(ch, (int32_t)ch->bright, duration_ms, 0);
    if (r != 0) {
        return r;
    }
    if (was_flow != 0u) {
        yglc_light_channel_start_flow(ch, NULL, 1u);
    }
    if (was_off == 1u) {
        yglc_light_channel_power_off(ch, 0u);
    }
    return 0;
}

/* 0x100c17b8 */
int yglc_light_channel_start_flow(yglc_light_channel_t *ch, const char *flow, uint32_t allow_switch)
{
    yglc_light_effect_t *e = NULL;
    yglc_effect_cbs_t cbs;
    lu_fill_channel_cbs(&cbs, ch, true);

    if (flow != NULL && strlen(flow) != 0u) {
        int r = yglc_light_effect_parse_flow_string(flow, &e);
        if (r != 0) {
            return r;
        }
        if (e->u.flow.count == 0 && flow != ch->flow_str) {   /* remember infinite flows */
            strncpy(ch->flow_str, flow, sizeof ch->flow_str);
            ch->flow_str[255] = '\0';
        }
    } else if (yglc_light_effect_parse_flow_string(ch->flow_str, &e) != 0) {
        return 0;
    }
    if (lu_list_empty(&ch->effects->stack)) {
        yglc_light_channel_restore(ch, LU_SETTINGS->trans_default_ms);
    }
    e->u.flow.allow_switch = allow_switch;
    yglc_light_effect_start(ch->effects, e, &cbs);
    return 0;
}

/* 0x100c1854 */
int yglc_light_channel_exit_nl(yglc_light_channel_t *ch, uint32_t duration_ms, uint32_t restore_saved)
{
    ch->is_nl = 0u;
    ch->restore_saved = restore_saved;
    return yglc_light_channel_restore(ch, duration_ms);
}

/* 0x100c1868: push an OFF effect (fade to 0) over whatever the channel shows. */
int yglc_light_channel_power_off(yglc_light_channel_t *ch, uint32_t duration_ms)
{
    yglc_effect_cbs_t cbs;
    yglc_light_effect_t *e = yglc_light_effect_create_new(YGLC_EFFECT_OFF);
    if (e == NULL) {
        return 5;
    }
    e->u.tuple.bright = 0u;
    e->u.tuple.duration_ms = duration_ms;
    lu_fill_channel_cbs(&cbs, ch, false);
    if (lu_list_empty(&ch->effects->stack)) {        /* need something to come back to */
        yglc_light_channel_restore(ch, LU_SETTINGS->trans_default_ms);
    }
    yglc_light_effect_start(ch->effects, e, &cbs);
    return 0;
}

/* 0x100c18d0: drop OFF/STATIC_8 and resume what is underneath. */
int yglc_light_channel_power_on(yglc_light_channel_t *ch, uint32_t duration_ms)
{
    yglc_light_effect_pop_types(ch->effects, YGLC_EFFECT_OFF | YGLC_EFFECT_STATIC_8);
    yglc_light_channel_t *dl = LU_SETTINGS->unit->channel[0];
    if (dl->skip_resume_once != 0u) {
        dl->skip_resume_once = 0u;
    } else if (!lu_list_empty(&ch->effects->stack)) {
        yglc_light_effect_resume(ch->effects, (int32_t)duration_ms);
        return 0;
    }
    ch->off = 0u;
    yglc_light_channel_restore(ch, duration_ms);
    return 0;
}

/* =====================================================================================
 * Light unit
 * ===================================================================================== */

/* 0x100c1920 */
yglc_light_unit_t *yglc_new_light_unit(const char *name)
{
    yglc_light_unit_t *u = (yglc_light_unit_t *)pvPortCalloc(1, sizeof *u);
    if (u == NULL) {
        LU_ERR("[E] failed to malloc for light channel\r\n (%s,%d)", "yglc_new_light_unit", 0x7f5);
        return NULL;
    }
    strncpy(u->name, name, 15);
    return u;
}

/* 0x100c1978: active channel if it supports the mode, else the first that does, else active. */
int32_t yglc_light_unit_find_channel(const yglc_light_unit_t *u, uint32_t mode)
{
    uint32_t bit = 1u << (mode & 0xffu);
    if ((lu_active_channel(u)->mode_mask & bit) != 0u) {
        return u->active;
    }
    for (int32_t i = 0; i < u->channel_count; i++) {
        if ((u->channel[i]->mode_mask & bit) != 0u) {
            return i;
        }
    }
    return u->active;
}

/* 0x100c19bc: channel cb +0x17c - move a flow to the channel that supports its next node. */
int yglc_light_unit_on_flow_step(yglc_light_unit_t *u, yglc_light_channel_t *ch, yglc_effect_flow_t *f)
{
    yglc_light_effect_t *fe = ch->effects->top;
    yglc_effect_flow_t *ff = &fe->u.flow;
    uint32_t mode;

    if (ff->count > 0 && ff->iter >= ff->count) {
        return 0;
    }
    yglc_lu_list_t *next = lu_flow_peek_next(ff);
    if (next == NULL || lu_link_is_null_node(next)) {
        return 0;
    }
    yglc_flow_node_t *node = lu_node_of(next);
    int32_t old = u->active;

    switch (node->mode) {
    case YGLC_CMODE_RGB:
    case YGLC_CMODE_FLOW:     mode = YGLC_CMODE_RGB;   break;
    case YGLC_CMODE_CT:
    case YGLC_CMODE_CT_BR:    mode = YGLC_CMODE_CT;    break;
    case YGLC_CMODE_NL_FLOW:  mode = YGLC_CMODE_SLEEP; break;
    default:                  return 0;
    }
    int32_t idx = yglc_light_unit_find_channel(u, mode);
    if (old == idx) {
        return 0;
    }
    if (u->flow_switched == 0u && f->action == YGLC_FLOW_END_RECOVER) {
        u->flow_switched = 1u;
        u->flow_saved_active = old;
        u->flow_saved_nl_status = u->nl_status;
    }

    /* unlink the flow effect from the old channel (no empty check, no stop) */
    fe = ch->effects->top;
    lu_list_unlink_init(&fe->stack);
    ch->effects->top = lu_effect_of_stack(ch->effects->stack.next);
    ch->flow_active = 0u;
    ch->off = 1u;
    u->active = idx;

    yglc_light_channel_t *nch = lu_active_channel(u);
    yglc_effect_cbs_t cbs;
    lu_fill_channel_cbs(&cbs, nch, true);
    yglc_light_effect_pop_types(nch->effects, YGLC_EFFECT_OFF);
    yglc_light_effect_start(nch->effects, fe, &cbs);
    yglc_light_channel_power_off(ch, node->tuple.duration_ms);
    return 1;
}

/* 0x100c1ad0: channel cb +0x184 - recompute the reported unit state. */
void yglc_light_unit_on_channel_state(yglc_light_unit_t *u, yglc_light_channel_t *ch)
{
    int32_t count = u->channel_count;

    if (u->nl_type == YGLC_NL_TYPE_CHANNEL) {
        if (u->active == 1) {
            u->nl_br_effective = u->channel[1]->bright;
            u->nl_active = 1u;
            u->active_mode = 1u;
        } else {
            u->nl_br_effective = 0u;
            u->nl_active = 0u;
            u->nl_status = 0u;            /* daylight shown -> forget "power on to nl" */
            u->active_mode = 0u;
        }
        u->active_bright = lu_active_channel(u)->bright;
    } else if (u->nl_type == YGLC_NL_TYPE_RGB_SLEEP || u->nl_type == YGLC_NL_TYPE_CT_SLEEP) {
        uint32_t mode = (u->nl_type == YGLC_NL_TYPE_RGB_SLEEP) ? YGLC_CMODE_RGB : YGLC_CMODE_CT;
        yglc_light_channel_t *c = u->channel[yglc_light_unit_find_channel(u, mode)];
        if (c->is_nl == 1u) {
            u->nl_br_effective = 1u;
            u->nl_active = 1u;
            u->active_bright = 1u;
            u->active_mode = 1u;
        } else {
            u->nl_br_effective = 0u;
            u->nl_active = 0u;
            u->nl_status = 0u;
            u->active_bright = c->bright;
            u->active_mode = 0u;
        }
    }

    yglc_light_channel_t *a = (count > 1) ? u->channel[u->active] : u->channel[0];
    if (a == ch) {
        u->off = ch->off;
        u->flowing = ch->flow_active;
        if (u->flowing != 0u) {
            strncpy(u->flow_params, ch->flow_str, sizeof u->flow_params);
        }
    }
    yglc_unit_state_notify();
}

/* 0x100c1b94 */
int yglc_light_unit_restore_all(yglc_light_unit_t *u, uint32_t duration_ms)
{
    for (int32_t i = 0; i < u->channel_count; i++) {
        u->channel[i]->off = (i == u->active) ? u->off : 1u;
        yglc_light_channel_restore(u->channel[i], duration_ms);
    }
    return 0;
}

/* 0x100c1bc6: channel cb +0x180 - flow finished (or stopped). */
void yglc_light_unit_on_flow_end(yglc_light_unit_t *u, yglc_light_channel_t *ch, yglc_effect_flow_t *f)
{
    lu_power_req_t req;

    memset(&req, 0, sizeof req);
    req.dur_ms = 500u;
    u->flowing = 0u;
    ch->flow_active = 0u;
    req.has_dur = 1u;

    switch (f->action) {
    case YGLC_FLOW_END_RECOVER:
        if (u->flow_switched != 0u) {
            u->flow_switched = 0u;
            yglc_light_effect_t *fe = ch->effects->top;
            lu_list_unlink_init(&fe->stack);              /* BUG: fe is never freed */
            ch->effects->top = lu_effect_of_stack(ch->effects->stack.next);
            ch->flow_active = 0u;
            ch->off = 1u;
            u->active = u->flow_saved_active;
            u->nl_status = u->flow_saved_nl_status;
            yglc_light_unit_restore_all(u, 500u);
            /* BUG: "movs r3,#0; ldr r1,[r3,#0x10]" - duration read from address 0x10
             * (vector table / boot ROM); clamped to [300 ms, 1 h] by the trans manager. */
            yglc_light_channel_power_off(ch, *(const volatile uint32_t *)(uintptr_t)0x10u);
        } else {
            yglc_light_effect_pop(ch->effects, 1);
        }
        break;
    case YGLC_FLOW_END_STAY:
        yglc_light_channel_commit_tuple(ch, &lu_node_of(f->cur)->tuple, 1);
        yglc_light_effect_pop_types(ch->effects, YGLC_EFFECT_FLOW);
        yglc_light_unit_restore_all(u, 500u);
        break;
    case YGLC_FLOW_END_OFF:
        yglc_light_effect_pop_types(ch->effects, YGLC_EFFECT_FLOW);
        req.mask = 1u << (u->unit_index & 0xffu);
        yglc_post_request_async(2u /* power off */, &req, sizeof req, 0u, 0u);
        break;
    case YGLC_FLOW_END_OFF_1:
        yglc_light_effect_pop_types(ch->effects, YGLC_EFFECT_FLOW);
        req.mask = 1u;
        yglc_post_request_async(2u /* power off */, &req, sizeof req, 0u, 0u);
        break;
    default:
        break;
    }
}

/* 0x100c1cb0 */
void yglc_light_unit_attach_channel(yglc_light_unit_t *u, int32_t idx, yglc_light_channel_t *ch)
{
    u->channel[idx] = ch;
    ch->on_flow_end = yglc_light_unit_on_flow_end;
    ch->on_flow_step = yglc_light_unit_on_flow_step;
    ch->owner = u;
    ch->on_state_change = yglc_light_unit_on_channel_state;
}

/* 0x100c1cdc: make idx active; it is lit unless the unit is off, the other one goes dark. */
int yglc_light_unit_activate(yglc_light_unit_t *u, int32_t idx, uint32_t duration_ms)
{
    u->active = idx;
    for (int32_t i = 0; i < 2; i++) {            /* fixed 2, not channel_count */
        if (i == u->active && u->off == 0u) {
            yglc_light_channel_power_on(u->channel[i], duration_ms);
        } else {
            yglc_light_channel_power_off(u->channel[i], duration_ms);
        }
    }
    return 0;
}

/* 0x100c1d12 */
int yglc_light_unit_set_power(yglc_light_unit_t *u, uint32_t off, uint32_t duration_ms)
{
    u->off = off;
    for (int32_t i = 0; i < u->channel_count; i++) {
        if (i == u->active && off == 0u) {
            yglc_light_channel_power_on(u->channel[i], duration_ms);
        } else {
            yglc_light_channel_power_off(u->channel[i], duration_ms);
        }
    }
    return 0;
}

/* 0x100c1d4a */
int yglc_light_unit_set_bright(yglc_light_unit_t *u, int32_t bright, uint32_t duration_ms, int as_type8)
{
    return yglc_light_channel_set_bright(lu_active_channel(u), bright, duration_ms, as_type8);
}

/* 0x100c1d64: stop_cf - run the flow end action now. */
int yglc_light_unit_stop_flow(yglc_light_unit_t *u)
{
    yglc_light_channel_t *ch = lu_active_channel(u);
    if (ch->flow_active != 0u && ch->effects->top->type == YGLC_EFFECT_FLOW) {
        yglc_light_unit_on_flow_end(u, ch, &ch->effects->top->u.flow);
    }
    return 0;
}

/* 0x100c1d90 */
bool yglc_light_unit_ct_in_range(yglc_light_unit_t *u, int32_t ct)
{
    yglc_light_channel_t *c = u->channel[yglc_light_unit_find_channel(u, YGLC_CMODE_CT)];
    if (ct > (int32_t)c->max_ct) {
        return false;
    }
    return ct >= (int32_t)c->min_ct;
}

/* 0x100c1db8: THE dl<->nl switch. Old channel fades out with duration_ms, the caller then
 * lights the new one with its own request. */
void yglc_light_unit_select_channel(yglc_light_unit_t *u, uint32_t mode, uint32_t duration_ms)
{
    int32_t idx = yglc_light_unit_find_channel(u, mode);
    if (idx == u->active) {
        return;
    }
    yglc_light_channel_t *old = lu_active_channel(u);
    u->active = idx;
    yglc_light_channel_power_off(old, duration_ms);
}

/* 0x100c1de6 */
int yglc_light_unit_set_raw(yglc_light_unit_t *u, const uint8_t rgbw[4], uint8_t arg)
{
    return yglc_light_channel_set_raw(lu_active_channel(u), rgbw, arg);
}

/* 0x100c1dfa */
int yglc_light_unit_set_rgb(yglc_light_unit_t *u, const uint32_t *rgb, int32_t bright, uint32_t duration_ms)
{
    yglc_light_unit_select_channel(u, YGLC_CMODE_RGB, duration_ms);
    return yglc_light_channel_set_rgb(lu_active_channel(u), rgb, bright, duration_ms);
}

/* 0x100c1e2e */
int yglc_light_unit_set_hsv(yglc_light_unit_t *u, const int32_t hs[2], int32_t bright, uint32_t duration_ms)
{
    yglc_light_unit_select_channel(u, YGLC_CMODE_HSV, duration_ms);
    return yglc_light_channel_set_hsv(lu_active_channel(u), hs, bright, duration_ms);
}

/* 0x100c1e62 (not in functions.csv) */
int yglc_light_unit_set_ct_keep_bright(yglc_light_unit_t *u, int32_t ct, uint32_t duration_ms)
{
    yglc_light_unit_select_channel(u, YGLC_CMODE_CT, duration_ms);
    return yglc_light_channel_set_ct(lu_active_channel(u), ct, -1, duration_ms);
}

/* 0x100c1e92 */
int yglc_light_unit_set_ct(yglc_light_unit_t *u, int32_t ct, int32_t bright, uint32_t duration_ms)
{
    yglc_light_unit_select_channel(u, YGLC_CMODE_CT, duration_ms);
    return yglc_light_channel_set_ct(lu_active_channel(u), ct, bright, duration_ms);
}

/* 0x100c1ec8 */
int yglc_light_unit_adjust_ct(yglc_light_unit_t *u, uint32_t action, uint32_t duration_ms,
                              uint32_t percent_of_range, int32_t delta)
{
    yglc_light_unit_select_channel(u, YGLC_CMODE_CT, duration_ms);
    yglc_light_channel_t *ch = lu_active_channel(u);
    int32_t ct = (int32_t)ch->ct;
    int32_t v;

    switch (action) {
    case YGLC_ADJ_TABLE_UP:
    case YGLC_ADJ_TABLE_DOWN:
        v = yglc_table_step(action, ct, ch->ct_table, ch->ct_table_len);
        break;
    case YGLC_ADJ_INCREASE:
    case YGLC_ADJ_DECREASE:
        if (percent_of_range == 1u) {
            yglc_light_channel_t *dl = LU_SETTINGS->unit->channel[0];
            int32_t min = (int32_t)dl->min_ct;
            int32_t range = (int32_t)dl->max_ct - min;
            int32_t pct = ((ct - min + range / 200) * 100) / range;
            if (action == YGLC_ADJ_INCREASE) {
                pct += delta;
                if (pct >= 100) {
                    pct = 100;
                }
            } else {
                pct -= delta;
                if (pct < 0) {
                    pct = 0;
                }
            }
            v = (pct * range) / 100;
            v = ((v + 5) / 10) * 10 + min;
        } else if (action == YGLC_ADJ_INCREASE) {
            v = (ct * (100 + delta)) / 100;
            if (v >= (int32_t)ch->max_ct) {
                v = (int32_t)ch->max_ct;
            }
        } else {
            v = (ct * (100 - delta)) / 100;
            if (v < (int32_t)ch->min_ct) {
                v = (int32_t)ch->min_ct;
            }
        }
        break;
    default:
        if (ch->ct_table == NULL) {
            LU_ERR("[E] missing brightness adjustment table\r\n (%s,%d)", "yglc_light_unit_adjust_ct", 0xa1b);
            return 1;
        }
        if (u->off == 1u) {
            return 0;
        }
        v = yglc_table_step(action, ct, ch->ct_table, ch->ct_table_len);
        break;
    }
    return yglc_light_unit_set_ct_keep_bright(u, v, duration_ms);
}

/* 0x100c2008: nl_type 0 moonlight bright ("nl_br"); switch_to_nl: activate nl now. */
int yglc_light_unit_nl_set_bright(yglc_light_unit_t *u, uint32_t bright, uint32_t duration_ms,
                                  int switch_to_nl)
{
    if (bright > 100u) {
        return 1;
    }
    if (bright == 0u) {
        bright = u->channel[1]->bright;
        if (bright == 0u) {
            bright = 5u;
        }
    }
    if (switch_to_nl) {
        u->active = 1;                                            /* set BEFORE dl fades */
        yglc_light_channel_power_off(u->channel[0], duration_ms);
    }
    if (u->active == 1) {
        return yglc_light_unit_set_bright(u, (int32_t)bright, duration_ms, 0);
    }
    u->channel[1]->bright = bright;                               /* just remember it */
    return 0;
}

/* 0x100c2050 */
int yglc_light_unit_enable_nl(yglc_light_unit_t *u, uint32_t bright, uint32_t duration_ms,
                              uint32_t set_status)
{
    switch (u->nl_type) {
    case YGLC_NL_TYPE_CHANNEL: {
        int r = yglc_light_unit_nl_set_bright(u, bright, duration_ms, 1);
        if (r == 0 && set_status == 1u) {
            u->nl_status = 1u;
        }
        return r;
    }
    case YGLC_NL_TYPE_RGB_SLEEP:
        yglc_light_unit_select_channel(u, YGLC_CMODE_RGB, 300u);
        return yglc_light_channel_set_nl_rgb(u->channel[u->active], duration_ms);
    case YGLC_NL_TYPE_CT_SLEEP:
        yglc_light_unit_select_channel(u, YGLC_CMODE_CT, 300u);
        return yglc_light_channel_set_nl_ct(u->channel[u->active], duration_ms);
    default:
        return 1;
    }
}

/* 0x100c20b4 */
void yglc_light_unit_disable_nl(yglc_light_unit_t *u, uint32_t duration_ms, uint32_t clear_status,
                                uint32_t restore_saved)
{
    int r;
    yglc_light_channel_t *c;

    switch (u->nl_type) {
    case YGLC_NL_TYPE_CHANNEL:
        r = yglc_light_unit_activate(u, 0, duration_ms);
        break;
    case YGLC_NL_TYPE_RGB_SLEEP:
        yglc_light_unit_select_channel(u, YGLC_CMODE_RGB, 300u);
        c = u->channel[u->active];
        c->is_nl = 0u;
        c->restore_saved = restore_saved;
        r = yglc_light_channel_restore(c, duration_ms);
        break;
    case YGLC_NL_TYPE_CT_SLEEP:
        yglc_light_unit_select_channel(u, YGLC_CMODE_CT, 300u);
        c = u->channel[u->active];
        c->is_nl = 0u;
        c->restore_saved = 0u;
        r = yglc_light_channel_restore(c, duration_ms);
        break;
    default:
        LU_ERR("[E] %s ERR nl_type=%d\r\n (%s,%d)", "yglc_light_unit_disable_nl", u->nl_type,
               "yglc_light_unit_disable_nl", 0xa7c);
        r = 0;
        break;
    }
    if (r != 0) {
        return;
    }
    if (clear_status == 1u) {
        u->nl_status = 0u;
    }
}

static int lu_adjust_rgb_common(yglc_light_unit_t *u, uint32_t duration_ms, const uint32_t *table,
                                int32_t len, volatile int32_t *gidx, const char *fn, int line)
{
    yglc_light_channel_t *ch = lu_active_channel(u);
    if (table == NULL) {
        LU_ERR("[E] missing brightness adjustment table\r\n (%s,%d)", fn, line);
        return 1;
    }
    uint32_t rgb = ch->rgb;
    int32_t idx = *gidx;
    uint32_t d = yglc_rgb_distance(rgb, table[idx]);
    if ((int32_t)d > 50) {
        for (int32_t i = 1; i < len; i++) {
            int32_t j = (i + idx) % len;
            if ((int32_t)yglc_rgb_distance(rgb, table[j]) < (int32_t)d) {
                idx = j;
                break;
            }
        }
    }
    idx = (idx + 1) % len;
    *gidx = idx;
    uint32_t next = table[idx];
    return yglc_light_unit_set_rgb(u, &next, -1, duration_ms);
}

/* 0x100c2164 (action ignored) */
int yglc_light_unit_adjust_rgb_new(yglc_light_unit_t *u, uint32_t action, uint32_t duration_ms)
{
    (void)action;
    yglc_light_unit_select_channel(u, YGLC_CMODE_RGB, duration_ms);
    yglc_light_channel_t *ch = lu_active_channel(u);
    return lu_adjust_rgb_common(u, duration_ms, ch->rgb_new_table, (int32_t)ch->rgb_new_table_len,
                                &LU_RGB_NEW_IDX, "yglc_light_unit_adjust_rgb_new", 0xaa7);
}

/* 0x100c2248 (action ignored) */
int yglc_light_unit_adjust_rgb(yglc_light_unit_t *u, uint32_t action, uint32_t duration_ms)
{
    (void)action;
    yglc_light_unit_select_channel(u, YGLC_CMODE_RGB, duration_ms);
    yglc_light_channel_t *ch = lu_active_channel(u);
    return lu_adjust_rgb_common(u, duration_ms, ch->rgb_table, (int32_t)ch->rgb_table_len,
                                &LU_RGB_IDX, "yglc_light_unit_adjust_rgb", 0xad3);
}

/* 0x100c232c (not in functions.csv); mode 4 matches no channel -> no switch. */
int yglc_light_unit_start_flow(yglc_light_unit_t *u, const char *flow, uint32_t allow_switch)
{
    if (u->flowing == 0u) {
        u->flow_switched = 0u;
    }
    yglc_light_unit_select_channel(u, YGLC_CMODE_FLOW, 300u);
    return yglc_light_channel_start_flow(lu_active_channel(u), flow, allow_switch);
}

/* 0x100c2366 */
void yglc_light_unit_flow_adjust_speed(yglc_light_unit_t *u, int32_t delta_ms)
{
    yglc_light_channel_flow_adjust_speed(lu_active_channel(u), delta_ms);
}

/* 0x100c237c: cron "off after N minutes" for nighttime power-on (request 0x15). */
int yglc_light_unit_arm_night_autooff(void)
{
    struct {
        uint8_t  type;          /* +0x00 0 = off job */
        uint8_t  _pad[3];
        uint32_t _unk_04;       /* +0x04 0 */
        uint32_t minutes;       /* +0x08 */
        uint32_t _unk_0c[2];
    } req;
    yglc_lu_settings_t *s = LU_SETTINGS;

    memset(&req, 0, sizeof req);
    s->night_autooff_armed = 1u;
    req.minutes = s->night_autooff_min;
    yglc_post_request_async(0x15u, &req, 0x14u, 0u, 0u);
    return 0;
}

/* 0x100c23b0: power on from off - daylight or moonlight. */
int yglc_light_unit_power_on(yglc_light_unit_t *u, uint32_t duration_ms)
{
    if (u->nl_type == YGLC_NL_TYPE_NONE ||
        (yglc_is_night_time() == 0u && u->nl_status != 1u)) {
        /* daylight */
        if ((uint32_t)u->nl_type - 1u <= 1u && u->channel[u->active]->is_nl == 1u) {
            yglc_light_unit_select_channel(u, u->saved_req.mode, 300u);
            yglc_light_channel_exit_nl(u->channel[u->active], duration_ms, 1u);
        }
        u->active = 0;
        return yglc_light_unit_set_power(u, 0u, duration_ms);
    }
    /* night time (or nl_status == 1) */
    if (yglc_is_night_time() != 0u && LU_SETTINGS->night_autooff_min != 0u) {
        yglc_light_unit_arm_night_autooff();
    }
    return yglc_light_unit_enable_nl(u, 0u, duration_ms, 0u);
}

/* 0x100c243c: set_power on with "mode". */
int yglc_light_unit_set_power_on_mode(yglc_light_unit_t *u, uint32_t mode, uint32_t duration_ms)
{
    switch (mode) {
    case YGLC_PWR_ON_NORMAL:
        if (u->off != 0u) {
            yglc_light_unit_power_on(u, duration_ms);
        }
        return 0;
    case YGLC_PWR_ON_CT:
        return yglc_light_unit_set_ct_keep_bright(u, 0, duration_ms);
    case YGLC_PWR_ON_FLOW:
        return yglc_light_unit_start_flow(u, LU_EMPTY_FLOW_STR, 1u);
    case YGLC_PWR_ON_NL:
        return yglc_light_unit_enable_nl(u, 0u, duration_ms, 1u);
    default:
        return 1;
    }
}

/* 0x100c24a8 */
int yglc_light_unit_adjust_bright(yglc_light_unit_t *u, uint32_t action, uint32_t duration_ms, int32_t delta)
{
    yglc_light_channel_t *ch = lu_active_channel(u);
    int32_t b = (int32_t)ch->bright;

    if (action == YGLC_ADJ_INCREASE) {
        if (u->off == 1u) {                      /* "+" while off: switch on at delta % */
            ch->bright = (uint32_t)delta;
            yglc_light_channel_set_bright(ch, delta, duration_ms, 0);
            return 0;
        }
        b += delta;
        if (b >= 100) {
            b = 100;
        }
        return yglc_light_unit_set_bright(u, b, duration_ms, 0);
    }
    if (action == YGLC_ADJ_DECREASE) {
        if (u->off == 1u) {
            return 0;
        }
        b -= delta;
        if (b > 0) {
            return yglc_light_unit_set_bright(u, b, duration_ms, 0);
        }
        if (LU_SETTINGS->unit == u) {
            yglc_light_api_power_off(1u, duration_ms);
        }
        return 0;
    }
    if (ch->bright_table == NULL) {
        LU_ERR("[E] missing brightness adjustment table\r\n (%s,%d)", "yglc_light_unit_adjust_bright", 0x988);
        return 1;
    }
    if (u->off == 1u) {
        return 0;
    }
    b = yglc_table_step(action, b, ch->bright_table, ch->bright_table_len);
    return yglc_light_unit_set_bright(u, b, duration_ms, 0);
}

/* =====================================================================================
 * API wrappers (unit mask; only bit 0 = settings->unit exists)
 * ===================================================================================== */

static bool lu_mask_main(uint32_t mask)
{
    if (mask == 0u) {
        mask = 1u;
    }
    return (mask & 1u) != 0u;
}

/* 0x100c2484 */
int yglc_light_api_power_off(uint32_t mask, uint32_t duration_ms)
{
    if (lu_mask_main(mask)) {
        yglc_light_unit_set_power(LU_SETTINGS->unit, 1u, duration_ms);
    }
    return 0;
}

/* 0x100c2574 (not in functions.csv; called by FUN_100bdcfa = request 1) */
int yglc_light_api_power_on(uint32_t mask, uint32_t mode, uint32_t duration_ms)
{
    if (lu_mask_main(mask)) {
        yglc_light_unit_set_power_on_mode(LU_SETTINGS->unit, mode, duration_ms);
    }
    return 0;
}

/* 0x100c2594 */
int yglc_light_api_set_bright(uint32_t mask, int32_t bright, uint32_t duration_ms, int as_type8)
{
    if (lu_mask_main(mask)) {
        yglc_light_unit_set_bright(LU_SETTINGS->unit, bright, duration_ms, as_type8);
    }
    return 0;
}

/* 0x100c25b4 (not in functions.csv) */
int yglc_light_api_set_raw(uint32_t mask, const uint8_t rgbw[4], uint8_t arg)
{
    if (lu_mask_main(mask)) {
        yglc_light_unit_set_raw(LU_SETTINGS->unit, rgbw, arg);
    }
    return 0;
}

/* 0x100c25d4 (not in functions.csv) */
int yglc_light_api_set_rgb(uint32_t mask, uint32_t rgb, int32_t bright, uint32_t duration_ms)
{
    if (lu_mask_main(mask)) {
        yglc_light_unit_set_rgb(LU_SETTINGS->unit, &rgb, bright, duration_ms);
    }
    return 0;
}

/* 0x100c25fc */
int yglc_light_api_set_hsv(uint32_t mask, int32_t hue, int32_t sat, int32_t bright, uint32_t duration_ms)
{
    int32_t hs[2] = { hue, sat };
    if (lu_mask_main(mask)) {
        yglc_light_unit_set_hsv(LU_SETTINGS->unit, hs, bright, duration_ms);
    }
    return 0;
}

/* 0x100c2628 */
bool yglc_light_api_ct_in_range(uint32_t mask, int32_t ct)
{
    if (!lu_mask_main(mask)) {
        return true;
    }
    return yglc_light_unit_ct_in_range(LU_SETTINGS->unit, ct);
}

/* 0x100c2650 (not in functions.csv) */
int yglc_light_api_set_ct(uint32_t mask, int32_t ct, int32_t bright, uint32_t duration_ms)
{
    if (lu_mask_main(mask)) {
        yglc_light_unit_set_ct(LU_SETTINGS->unit, ct, bright, duration_ms);
    }
    return 0;
}

/* 0x100c2670 */
int yglc_light_api_adjust_ct(uint32_t mask, uint32_t action, uint32_t duration_ms,
                             uint32_t percent_of_range, int32_t delta)
{
    if (!lu_mask_main(mask)) {
        return 0;
    }
    return yglc_light_unit_adjust_ct(LU_SETTINGS->unit, action, duration_ms, percent_of_range, delta);
}

/* 0x100c269c */
int yglc_light_api_adjust_rgb(uint32_t mask, uint32_t action, uint32_t duration_ms,
                              uint32_t unused, uint32_t variant)
{
    (void)unused;
    if (lu_mask_main(mask)) {
        if (variant == 2u) {
            yglc_light_unit_adjust_rgb_new(LU_SETTINGS->unit, action, duration_ms);
        } else {
            yglc_light_unit_adjust_rgb(LU_SETTINGS->unit, action, duration_ms);
        }
    }
    return 0;
}

/* 0x100c26c8 */
int yglc_light_api_adjust_bright(uint32_t mask, uint32_t action, uint32_t duration_ms, int32_t delta)
{
    if (lu_mask_main(mask)) {
        yglc_light_unit_adjust_bright(LU_SETTINGS->unit, action, duration_ms, delta);
    }
    return 0;
}

/* =====================================================================================
 * Nighttime / settings glue (outside the range)
 * ===================================================================================== */

/* 0x100be560 "is_night_time" (prop, cached in settings+0xf8). */
uint32_t yglc_is_night_time(void)
{
    yglc_lu_settings_t *s = LU_SETTINGS;
    int32_t start = s->night_start_s;
    int32_t end = s->night_end_s;

    LU_INFO("[I] nighttime enabled : %d\n", s->nighttime_enabled);   /* FUN_100be520 */
    if (s->nighttime_enabled == 0u) {
        s->is_night_cached = 0u;
        return 0u;
    }
    if (LU_NTP_SYNCED == 0u) {                                         /* FUN_100be510 */
        LU_INFO("[I] time info not received from ntp server yet\n");
        return (s->is_night_cached != 0u) ? 1u : 0u;
    }

    int32_t tz = miio_get_tz_offset(LU_MIIO_HANDLE);
    int32_t off = ((uint32_t)(tz + 43200) <= 86400u) ? tz : 28800;    /* default UTC+8 */
    uint32_t now = arch_utc_seconds() + (uint32_t)off;
    int32_t tm[9];
    arch_gmtime_r(&now, tm);
    LU_INFO("[I] curtime:%02d:%02d:%02d\n", tm[2], tm[1], tm[0]);
    int32_t sec = (tm[2] * 60 + tm[1]) * 60 + tm[0];
    LU_INFO("[I] night = %d->%d, now= %d\n", start, end, sec);

    bool night;
    if (end > start) {
        night = (sec >= start) && (sec <= end);
    } else {
        night = (sec >= start) || (sec <= end);
    }
    if (night) {
        LU_INFO("[I] night time it is\n");
        s->is_night_cached = 1u;
        return 1u;
    }
    LU_INFO("[I] night time it is not\n");
    s->is_night_cached = 0u;
    return 0u;
}

/* 0x100be6bc (tail of the unit state callback). */
void yglc_unit_state_notify(void)
{
    yglc_lu_settings_t *s = LU_SETTINGS;
    yglc_light_unit_t *u = s->unit;
    uint32_t power_off = (u->off != 0u) ? 1u : (u->aux_power != 0u ? 1u : 0u);

    if (s->power_off != power_off) {
        s->power_off = power_off;
    }
    if (s->power_off == 1u) {
        yglc_cron_cancel_type(0u);
        yglc_power_state_hook(1u);
    } else {
        yglc_cron_cancel_type(1u);
        yglc_power_state_hook(0u);
    }
    if (s->night_autooff_armed != 0u && u->nl_active == 0u) {   /* left moonlight */
        yglc_cron_cancel_type(0u);
        s->night_autooff_armed = 0u;
    }
    /* FUN_100be6b8: bx lr */
    yglc_report_props(0u);
}

/* =====================================================================================
 * PROPOSAL (not in the original firmware): "hybrid" low end.
 *
 * Patch: BL @0x100c09ce (next_trans -> start_transition, the ONLY caller) re-targeted to
 * this function. While the daylight channel is active and its target bright is in
 * [1, K-1], the dl PWM target is forced to 0 and the moonlight PWM fades to f(bright) in
 * the same number of steps; at bright >= K (or off) nl fades out again. Unit/channel
 * state is untouched: app sees active_mode 0, bright = b; saved state stays dl@b, so
 * power-on re-enters the hybrid automatically. nl OFF effects that the unit pushes while
 * dl is active (set_power loops dl first, nl second) are re-mapped to the hybrid level.
 * Retarget path (0x100c07ba, bright change inside a running flow) is NOT covered.
 * ===================================================================================== */

/* K from the LED-driver measurement (LIGHT_MIXER.md): main LEDs are off below ~5.5 % duty
 * and sit on one constant minimum level up to ~10.5-11 % (~= nl_br 100). With the patched
 * mixer (#9a min_bright 0, #9b floor 6.5) dl bright N ~= 6.5 + 0.935*N % duty, so N = 1..4
 * (7.4..10.2 %) are all the same plateau and N = 5 (11.2 %) is the first real dimming step
 * -> K = 5, and bright 1..4 are handed to the moon LEDs (nl_br N = N % duty, linear).
 * With the STOCK mixer (min_bright 15, floor 8) dl bright 1 is already 21.8 % duty, so the
 * hybrid only makes sense together with #9a (otherwise there is a jump at K). */
#ifndef YGLC_HYBRID_K
#define YGLC_HYBRID_K 5u             /* dl bright below this -> moonlight LEDs        */
#endif
_Static_assert(YGLC_HYBRID_K >= 2u && YGLC_HYBRID_K <= 100u, "hybrid K range");

/* Zero-BSS state: trans_manager +0xdc of the dl channel is never referenced by
 * 0x100c02bc..0x100c2800 (checked in the disassembly) -> reuse it for the hybrid level. */
#define g_hybrid_nl_bright (dl_tm->_unk_dc)

/* dl bright 1..K-1 -> nl_br, quadratic (perceptually even), f(K-1) = 100 = plateau level:
 * K = 5 -> 7, 25, 57, 100. */
static uint8_t yglc_hybrid_map(uint8_t dl_bright)
{
    const uint32_t d = (YGLC_HYBRID_K - 1u) * (YGLC_HYBRID_K - 1u);
    uint32_t v = (100u * dl_bright * dl_bright + d - 1u) / d;
    return (uint8_t)(v > 100u ? 100u : v);
}

int yglc_hybrid_start_transition(yglc_trans_manager_t *tm, yglc_trans_tuple_t *t,
                                 const yglc_trans_cbs_t *cbs)
{
    yglc_light_unit_t *u = LU_SETTINGS->unit;
    yglc_trans_manager_t *dl_tm = u->channel[0]->effects->trans;
    yglc_trans_manager_t *nl_tm = u->channel[1]->effects->trans;

    if (u->active != 0) {                        /* real moonlight mode: hands off */
        g_hybrid_nl_bright = 0u;
        return yglc_trans_manager_start_transition(tm, t, cbs);
    }
    if (tm == nl_tm) {                           /* nl OFF while hybrid is shown */
        if (t->bright == 0u && g_hybrid_nl_bright != 0u) {
            yglc_trans_tuple_t keep = *t;
            keep.bright = (uint8_t)g_hybrid_nl_bright;
            keep.has_ct = 1u;
            return yglc_trans_manager_start_transition(tm, &keep, cbs);
        }
        return yglc_trans_manager_start_transition(tm, t, cbs);
    }
    if (tm != dl_tm) {
        return yglc_trans_manager_start_transition(tm, t, cbs);
    }

    uint8_t b = t->bright;
    uint32_t was = g_hybrid_nl_bright;
    g_hybrid_nl_bright = (b != 0u && b < YGLC_HYBRID_K) ? yglc_hybrid_map(b) : 0u;

    int r = yglc_trans_manager_start_transition(tm, t, cbs);
    if (g_hybrid_nl_bright != 0u) {
        memset(tm->target_pwm, 0, sizeof tm->target_pwm);   /* dl fades to dark */
    }
    if (g_hybrid_nl_bright != 0u || was != 0u) {
        yglc_trans_tuple_t nt;
        yglc_trans_cbs_t keep_cbs = nl_tm->cbs;              /* nl effect stays consistent */
        memset(&nt, 0, sizeof nt);
        nt.ct = (uint16_t)u->channel[1]->ct;
        nt.has_ct = 1u;
        nt.bright = (uint8_t)g_hybrid_nl_bright;             /* 0 -> fade out */
        nt.duration_ms = t->duration_ms;
        nt.step_ms = nl_tm->default_step_ms;
        yglc_trans_manager_start_transition(nl_tm, &nt, &keep_cbs);
    }
    return r;
}
