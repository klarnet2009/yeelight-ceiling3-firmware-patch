/*
 * Clean, human-readable C11 reconstruction of the light COMMAND layer of
 * Yeelight Ceiling3 (yeelink.light.ceiling3, 2.0.2_0048, MT7697 Cortex-M4F).
 *
 * Scope (see yglc_light_cmd_clean.h and decomp/LIGHT_CMD.md):
 *   - miIO/LAN JSON-RPC dispatcher and method handlers (0x100bbd44..0x100bd8c0)
 *   - request queue, yglc_task and yglc_proc_request (0x100bdad8..0x100bff30)
 *   - set_ps dispatcher, nighttime, night light set_ps
 *   - BLE remote / seesaw (0x100be728..0x100bf77c)
 *   - cold boot power decision + fast reboot counter
 *
 * The light-unit / channel / transition / mixer code (0x100c06a0..0x100c2600)
 * is NOT reconstructed here; it is called through typed externs below.
 *
 * Пометки:  BUG: - ошибка оригинальной прошивки (поведение сохранено или
 *           явно оговорено), ASSUMPTION: - вывод, не подтверждённый тестом.
 */

#include "../include/yglc_light_cmd_clean.h"

/* ---- libc (ROM symbols) ---- */
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strtok_r(char *s, const char *delim, char **save);            /* FUN_101093a2 */
int    snprintf(char *buf, size_t size, const char *fmt, ...);
int    sprintf(char *buf, const char *fmt, ...);
int    sscanf(const char *s, const char *fmt, ...);
long   strtol(const char *s, char **end, int base);
int    atoi(const char *s);

/* ---- FreeRTOS / arch_os / psm ---- */
void    *pvPortCalloc(size_t nmemb, size_t size);
void     vPortFree(void *p);
int      xQueueGenericSend(void *q, const void *item, uint32_t ticks, int pos);
int      xQueueGenericSendFromISR(void *q, const void *item, int32_t *woken, int pos);
int      xQueueGenericReceive(void *q, void *item, uint32_t ticks, int peek);
uint32_t arch_os_ms_now(void);
int      arch_os_timer_create(void **t, const char *name, uint32_t ms, void (*cb)(void *),
                              void *arg, int reload, int start);
int      arch_os_timer_activate(void *t);
int      psm_get_variable(const char *mod, const char *var, char *buf, int len);
int      psm_set_variable(const char *mod, const char *var, const char *val);
void     arch_dbg_log_begin(const char *esc);
void     arch_dbg_log_end(const char *esc);
int      log_printf(const char *fmt, ...);

/* ---- jsmi (miIO JSON helper) ---- */
typedef struct {
    const void *key;   /* const char * key, or (uintptr_t) array index */
    uint8_t     type;  /* 2 = array member, 3 = string member, 4 = index */
} jsmi_path_t;
int jsmi_get_value_uint32(void *jsmn, int base_tok, const jsmi_path_t *path, int depth, uint32_t *out);
int jsmi_get_value_str(void *jsmn, int base_tok, const jsmi_path_t *path, int depth, char *out, uint32_t cap);
int yglc_json_parser_init(yglc_rpc_parser_t **out, void *rpc_arg, yglc_rpc_reply_fn_t reply,
                          void *reply_arg);                                  /* 0x100bc64c */
void jsmi_parse_finish(void *jsmn);

/* ---- miIO reply / net ---- */
int  FUN_100f9f0c(void *rpc_hdr, yglc_rpc_reply_fn_t reply, void *arg);      /* miio_reply_ok */
int  FUN_100f9fa4(void *rpc_hdr, int code, const char *msg,
                  yglc_rpc_reply_fn_t reply, void *arg);                     /* miio_reply_error */
void lan_ctrl_init(void);                                                    /* 0x100c41e4 */
void FUN_100c45a6(void);                                                     /* lan_adv_start */
void FUN_100c458c(void);                                                     /* lan_ctrl_stop */
uint32_t FUN_100f715a(uint32_t miio);                                        /* miio_get_gmt_offset */
int32_t  FUN_100a8650(void);                                                 /* utc_seconds_now */
void     FUN_100a8fd4(const int32_t *t, int32_t *tm_sec_min_hour);           /* seconds -> {sec,min,hour} */
extern uint32_t g_miio_net;                                                  /* @0x20004b24 (+0 = miio handle) */

/* ---- yglc timers / state persistence / report (yl_cache.c etc.) ---- */
int  FUN_100c35f2(void *timer_ctl, uint64_t *next_ms);                       /* yglc_timer_next */
void yglc_timer_clock_init(void *timer_ctl);                                 /* 0x100c34fc */
void FUN_100c35b4(void *timer_ctl);                                          /* yglc_timer_run_expired */
void FUN_100c30b8(int periodic);                                             /* yglc_flush_state: report+psm(4)+nv */
void FUN_100c27b8(int force);                                                /* yglc_report_props */
void FUN_100c2ebc(uint8_t psm_mask);                                         /* yglc_psm_save(mask) */
void FUN_100c2eb0(void);                                                     /* yglc_save_state_changed */
uint8_t FUN_100c323c(void);                                                  /* warm boot type (1/2) */
int  FUN_100c3248(void);                                                     /* warm boot? (type 1..2) */
void FUN_100bb8e0(uint32_t period_ms, uint32_t count);                       /* yglc_indicator_blink */
uint32_t FUN_100bb8dc(const yglc_ps_req_t *ps);                              /* set_ps fallback (stub) */
void ble_evt_test_exit_cbk(uint32_t a);                                      /* 0x100beaa0 */
void FUN_100bd8c0(void *tmr);                                                /* wifi restore + reboot */

/* ---- cron (yglc_core.c 0x100bfff0..0x100c0194) ---- */
uint32_t FUN_100c00f0(const yglc_cron_req_t *r);                             /* cron_add_or_update */
uint32_t FUN_100c0158(uint32_t type);                                        /* cron_del_type */
uint32_t FUN_100c0194(const yglc_cron_sel_req_t *sel, void *out);            /* cron_get */

/* ---- light unit API (light-unit layer, NOT reconstructed here) ---- */
void     FUN_100c2574(uint32_t flags, uint32_t mode, uint32_t dur);          /* -> unit_power_on_mode 0x100c243c */
uint32_t FUN_100c2484(uint32_t flags, uint32_t dur);                         /* -> unit_set_off(unit,1,dur) 0x100c1d12 */
uint32_t FUN_100c2594(uint32_t flags, int32_t bright, uint32_t dur, uint32_t v2); /* -> unit_set_bright 0x100c1d4a */
int      FUN_100c2628(uint32_t flags, int32_t ct);                           /* ct inside CT channel range */
void     FUN_100c2650(uint32_t flags, int32_t ct, int32_t bright, uint32_t dur); /* -> 0x100c1e92 (switches to CT ch) */
void     FUN_100c25d4(uint32_t flags, uint32_t rgb, int32_t bright, uint32_t dur); /* -> 0x100c1dfa */
void     FUN_100c25b4(uint32_t flags, const void *rgbw, uint32_t arg);       /* -> 0x100c1de6 */
void     FUN_100c25fc(uint32_t flags, int32_t hue, int32_t sat, int32_t bright, uint32_t dur); /* -> 0x100c1e2e */
uint32_t FUN_100c2670(uint32_t flags, uint8_t action, uint32_t dur, uint8_t opt, int32_t value); /* adjust ct */
uint32_t FUN_100c269c(uint32_t flags, uint8_t action, uint32_t dur, int32_t value, uint8_t opt); /* adjust color */
uint32_t FUN_100c26c8(uint32_t flags, uint8_t action, uint32_t dur, int32_t value); /* yglc_light_unit_adjust_bright */
uint32_t FUN_100c26e8(const yglc_test_req_t *t);                             /* set_test raw */
void     FUN_100c1db8(yglc_unit_view_t *u, uint32_t cmode, uint32_t dur);    /* select channel for color mode */
uint32_t FUN_100c17b8(yglc_channel_view_t *ch, const char *expr, uint32_t opt); /* channel start flow */
uint32_t FUN_100c1d64(yglc_unit_view_t *u);                                  /* unit stop flow */
void     FUN_100c2366(yglc_unit_view_t *u);                                  /* unit flow string update */
uint32_t FUN_100c1d4a(yglc_unit_view_t *u, int32_t bright, uint32_t dur, uint32_t v2); /* set bright on ACTIVE channel */
int      FUN_100c2050(yglc_unit_view_t *u, uint32_t br, uint32_t dur, int save); /* NL enable (+0x12c=1 if save) */
uint32_t FUN_100c2008(yglc_unit_view_t *u, uint32_t br, uint32_t dur, int activate); /* NL set bright */
void     yglc_light_unit_disable_nl(yglc_unit_view_t *u, uint32_t dur, int save, uint32_t keep); /* 0x100c20b4 */

/* ---- product RPC handlers not covered here (kept as externs) ---- */
int FUN_100bcc40(yglc_rpc_parser_t *p); /* get_prop (synchronous, reads g_yl_cache_list) */
int FUN_100bd32c(yglc_rpc_parser_t *p); /* set_rgb  -> req 6 */
int FUN_100bd38c(yglc_rpc_parser_t *p); /* set_hsv  -> req 7 */
int FUN_100bd3ec(yglc_rpc_parser_t *p); /* set_wrgb -> req 9 */
int FUN_100bd44c(yglc_rpc_parser_t *p); /* set_xy   -> req 8 (unsupported) */
int FUN_100bcbb0(yglc_rpc_parser_t *p); /* adjust_color -> req 0x1b prop 6 */
int FUN_100bcff4(yglc_rpc_parser_t *p); /* set_test -> req 0x21 */
int FUN_100bc930(yglc_rpc_parser_t *p); /* cron_del -> req 0x16 */
int FUN_100bc994(yglc_rpc_parser_t *p); /* cron_get -> req 0x17 */
int FUN_100bcf1c(yglc_rpc_parser_t *p); /* set_music -> req 0x1d */
int FUN_100bcf98(yglc_rpc_parser_t *p); /* set_name (strcpy + req 0x26 twice) */
int FUN_100bbc0e(yglc_rpc_parser_t *p); /* set_indicator: return 0, NO reply (BUG) */
int FUN_100bceb8(yglc_rpc_parser_t *p); /* get_ps -> req 0x1a */
int FUN_100bbc0a(yglc_rpc_parser_t *p); /* set_alert: return 0, NO reply (BUG) */
int FUN_100bc3f8(yglc_rpc_parser_t *p); /* miio_test */
int FUN_100bc9f4(yglc_rpc_parser_t *p); /* miio_test_exit */
int FUN_100bbc14(yglc_rpc_parser_t *p); /* get_aging_status */
int FUN_100bbcc0(yglc_rpc_parser_t *p); /* suicide */
int FUN_100bc46c(yglc_rpc_parser_t *p); /* ble_dbg_tbl_dump */

/* ---- Globals ---- */
extern yglc_settings_view_t *g_yglc_settings_ptr;          /* @0x20006908 */
extern int32_t g_log_level;                                /* @0x20004a2c */
extern yglc_core_ctx_t g_yglc_core;                        /* @0x20007fc4 */
extern yglc_req_slot_t g_yglc_req_slots[YGLC_REQ_SLOT_COUNT]; /* @0x20006918 (memset 0x1630) */
extern void *g_yglc_timer_ctl;                             /* @0x20008020 */
extern uint8_t g_seesaw_volley;                            /* @0x20008024 */
extern uint32_t g_ble_factory_seq_idx;                     /* @0x20007fbc */
extern uint32_t g_ble_sleep_prev;                          /* @0x20000c78 */
extern int32_t g_fast_reboot_count;                        /* @0x20000c80 */
extern int32_t g_factory_passed_cache;                     /* @0x20000c7c (-1 = unknown) */
extern void   *g_fast_reboot_zero_tmr;                     /* @0x20007fc0 */
extern void   *g_prov_reset_tmr;                           /* @0x20007fac */
extern uint32_t g_ntp_time_valid;                          /* @0x20007fb8 */
extern const char *const g_lan_exposed_methods[16];        /* @0x20000a34 (count 15 @0x20000a30 is the BLE table size) */

#define S()     (g_yglc_settings_ptr)
#define UNIT()  (g_yglc_settings_ptr->unit)

#define LOGE(...) do { if (g_log_level < 4) { arch_dbg_log_begin("\x1b[0;31m"); \
                        log_printf(__VA_ARGS__); arch_dbg_log_end("\x1b[0m"); } } while (0)
#define LOGI(...) do { if (g_log_level < 2) { arch_dbg_log_begin("\x1b[0;32m"); \
                        log_printf(__VA_ARGS__); arch_dbg_log_end("\x1b[0m"); } } while (0)

static inline yglc_channel_view_t *unit_active_ch(const yglc_unit_view_t *u)
{
    /* Original: nchan < 2 ? ch[0] : ch[active] */
    return (u->nchan < 2) ? u->ch[0] : u->ch[u->active];
}

/* ===================================================================== */
/*  1. JSON-RPC helpers                                                   */
/* ===================================================================== */

/* 0x100bc864: read params[param_idx] as uint32, advance index on success. */
int yglc_rpc_pop_int(yglc_rpc_parser_t *p, int32_t *out)
{
    const jsmi_path_t path[2] = {
        { "params", 2 },
        { (const void *)(uintptr_t)p->param_idx, 4 },
    };
    uint32_t v = 0;
    if (jsmi_get_value_uint32(p->jsmn, 0, path, 2, &v) != 0) {
        return -1;
    }
    *out = (int32_t)v;
    p->param_idx++;
    return 0;
}

/* 0x100bcc00: read params[param_idx] as string. */
int yglc_rpc_pop_str(yglc_rpc_parser_t *p, char *out, uint32_t cap)
{
    const jsmi_path_t path[2] = {
        { "params", 2 },
        { (const void *)(uintptr_t)p->param_idx, 3 },
    };
    if (jsmi_get_value_str(p->jsmn, 0, path, 2, out, cap) != 0) {
        return -1;
    }
    p->param_idx++;
    return 0;
}

static uint8_t effect_from_str(const char *s)
{
    if (strncmp(s, "smooth", 7) == 0) return YGLC_EFFECT_SMOOTH;
    if (strncmp(s, "curvy", 6) == 0)  return YGLC_EFFECT_CURVY;
    if (strncmp(s, "linear", 7) == 0) return YGLC_EFFECT_LINEAR;
    return YGLC_EFFECT_SUDDEN; /* "sudden" and anything unknown */
}

/* 0x100bd074: optional ["effect", duration]. No string -> defaults; string
 * without duration -> -1. Duration is NOT range checked here. */
int yglc_rpc_parse_effect_duration(yglc_rpc_parser_t *p, uint8_t *effect, uint8_t dflt_effect,
                                   uint32_t *duration, uint32_t dflt_duration)
{
    char eff[20];
    int32_t dur;
    if (yglc_rpc_pop_str(p, eff, 0x10) != 0) {
        *effect = dflt_effect;
        *duration = dflt_duration;
        return 0;
    }
    if (yglc_rpc_pop_int(p, &dur) != 0) {
        return -1;
    }
    *effect = effect_from_str(eff);
    *duration = (uint32_t)dur;
    return 0;
}

/* 0x100bd160: accepts either [duration, "effect"] or ["effect", duration]. */
static int rpc_parse_duration_effect_any(yglc_rpc_parser_t *p, uint8_t *effect, uint8_t dflt_effect,
                                         uint32_t *duration, uint32_t dflt_duration)
{
    char eff[16];
    int32_t dur;
    if (yglc_rpc_pop_int(p, &dur) == 0) {
        *duration = (uint32_t)dur;
        *effect = (yglc_rpc_pop_str(p, eff, 0x10) == 0) ? effect_from_str(eff) : dflt_effect;
        return 0;
    }
    if (yglc_rpc_pop_str(p, eff, 0x10) != 0) {
        *effect = dflt_effect;
        *duration = dflt_duration;
        return 0;
    }
    *effect = effect_from_str(eff);
    return (yglc_rpc_pop_int(p, &dur) == 0) ? (*duration = (uint32_t)dur, 0) : -1;
}

/* 0x100bd234: [v1, (v2), ("effect", duration)] - v1/v2 mandatory when requested. */
static int rpc_pop_values_effect(yglc_rpc_parser_t *p, int32_t *v1, int32_t *v2, uint8_t *effect,
                                 uint8_t dflt_effect, uint32_t *dur, uint32_t dflt_dur)
{
    if (v1 != NULL && yglc_rpc_pop_int(p, v1) != 0) {
        LOGE("[E] missing param 1\n (%s,%d)", "yglc_json_parser_pop_request_2params", 0x20c);
        return -1;
    }
    if (v2 != NULL && yglc_rpc_pop_int(p, v2) != 0) {
        LOGE("[E] missing param 2\n (%s,%d)", "yglc_json_parser_pop_request_2params", 0x211);
        return -1;
    }
    if (effect == NULL) {
        return 0;
    }
    return (yglc_rpc_parse_effect_duration(p, effect, dflt_effect, dur, dflt_dur) != 0) ? -1 : 0;
}

/* 0x100bd50c: same as above but effect/duration accepted in either order. */
static int rpc_pop_values_effect_any(yglc_rpc_parser_t *p, int32_t *v1, int32_t *v2, uint8_t *effect,
                                     uint8_t dflt_effect, uint32_t *dur, uint32_t dflt_dur)
{
    if (v1 != NULL && yglc_rpc_pop_int(p, v1) != 0) return -1;
    if (v2 != NULL && yglc_rpc_pop_int(p, v2) != 0) return -1;
    if (effect == NULL || dur == NULL) return 0;
    return (rpc_parse_duration_effect_any(p, effect, dflt_effect, dur, dflt_dur) != 0) ? -1 : 0;
}

/* 0x100bc33c: map status to miIO reply. Error code sent = -5000 - status. */
int yglc_rpc_reply_status(int status, void *rpc_hdr, yglc_rpc_reply_fn_t reply, void *arg)
{
    static const char *const k_err[6] = {
        "invalid params", "hardware fault", "timed out",
        "general error", "memory exhausted", "illegal request",
    };
    if (status == 0) {
        FUN_100f9f0c(rpc_hdr, reply, arg);
    } else {
        uint32_t i = (uint32_t)(status - 1) & 0xffu;
        const char *msg = (i < 6u) ? k_err[i] : "internal error";
        FUN_100f9fa4(rpc_hdr, -5000 - status, msg, reply, arg);
    }
    return status;
}

/* 0x100bc384 */
void yglc_rpc_parser_free(yglc_rpc_parser_t *p)
{
    jsmi_parse_finish(p->jsmn);
    vPortFree(p);
}

static int rpc_fail(yglc_rpc_parser_t *p, int status)
{
    yglc_rpc_reply_status(status, p->rpc_hdr, p->reply, p->reply_arg);
    yglc_rpc_parser_free(p);
    return status;
}

/* 0x100bc44c: completion callback of all asynchronous methods (runs in yglc_task). */
void yglc_rpc_on_request_done(uint32_t status, void *out, void *parser)
{
    yglc_rpc_parser_t *p = (yglc_rpc_parser_t *)parser;
    (void)out;
    if (p->reply != NULL && p->reply_arg != NULL) {
        yglc_rpc_reply_status((int)status, p->rpc_hdr, p->reply, p->reply_arg);
    }
    yglc_rpc_parser_free(p);
}

/* 0x100bbd44: post a request whose completion replies to the RPC caller.
 * Returns 0 (posted), 5 (queue/slots full) or 7 (no callback). On non-zero
 * the CALLER must reply and free the parser. */
int json_rpc_to_yglc(uint8_t req_id, const void *payload, uint32_t len,
                     yglc_req_done_cb_t cb, yglc_rpc_parser_t *p)
{
    if (cb == NULL) {
        LOGE("[E] a callback must be provided to free parser\n (%s,%d)", "json_rpc_to_yglc", 0x249);
        return YGLC_ERR_INTERNAL;
    }
    return (yglc_post_request_async(req_id, payload, len, cb, p) >= 0) ? YGLC_OK : YGLC_ERR_NOMEM;
}

/* Post and reply on failure: pattern used by nearly every handler. */
static int rpc_post_or_fail(yglc_rpc_parser_t *p, uint8_t id, const void *pl, uint32_t len)
{
    int r = json_rpc_to_yglc(id, pl, len, yglc_rpc_on_request_done, p);
    return (r == 0) ? 0 : rpc_fail(p, r);
}

/* ===================================================================== */
/*  2. Method handlers                                                    */
/* ===================================================================== */

/* 0x100bd0f0 set_power ["on"|"off", "effect", duration, mode]
 * Request: 1 (on) for exactly "on", 2 (off) for ANY other string.
 * BUG: if params[0] is not a string the request id register is left
 *      uninitialised (random request posted); here defaulted to OFF.
 * BUG: parse errors of effect/duration are ignored (no error reply); mode is
 *      never range-checked (2/3/>5 silently do nothing but answer "ok"). */
int yglc_m_set_power(yglc_rpc_parser_t *p)
{
    yglc_power_req_t r;
    char onoff[16];
    uint8_t id = YGLC_REQ_POWER_OFF;
    int32_t mode;

    memset(&r, 0, sizeof r);
    r.flags = 1;
    if (yglc_rpc_pop_str(p, onoff, 0x10) == 0) {
        id = (strncmp(onoff, "on", 3) == 0) ? YGLC_REQ_POWER_ON : YGLC_REQ_POWER_OFF;
        if (yglc_rpc_parse_effect_duration(p, &r.effect, YGLC_EFFECT_SMOOTH, &r.duration_ms, 500) >= 0 &&
            yglc_rpc_pop_int(p, &mode) == 0) {
            r.mode = (uint32_t)mode;
        }
    }
    /* original ignores the post result (parser leaks if the queue is full) */
    return json_rpc_to_yglc(id, &r, sizeof r, yglc_rpc_on_request_done, p);
}

/* 0x100bc3c8 toggle [] */
int yglc_m_toggle(yglc_rpc_parser_t *p)
{
    return rpc_post_or_fail(p, YGLC_REQ_TOGGLE, NULL, 0);
}

/* 0x100bc398 set_default [] */
int yglc_m_set_default(yglc_rpc_parser_t *p)
{
    return rpc_post_or_fail(p, YGLC_REQ_SET_DEFAULT, NULL, 0);
}

/* 0x100bd2cc set_bright [bright, "effect", duration]; range 1..100 is
 * checked later in yglc_proc_request (reply "invalid params"). */
int yglc_m_set_bright(yglc_rpc_parser_t *p)
{
    yglc_bright_req_t r;
    memset(&r, 0, sizeof r);
    r.flags = 1;
    if (rpc_pop_values_effect(p, &r.bright, NULL, &r.effect, YGLC_EFFECT_SMOOTH,
                              &r.duration_ms, 500) != 0) {
        return rpc_fail(p, YGLC_ERR_INVALID);
    }
    return rpc_post_or_fail(p, YGLC_REQ_SET_BRIGHT, &r, sizeof r);
}

/* 0x100bd4ac set_ct_abx [ct, "effect", duration] */
int yglc_m_set_ct_abx(yglc_rpc_parser_t *p)
{
    yglc_ct_req_t r;
    memset(&r, 0, sizeof r);
    r.flags = 1;
    if (rpc_pop_values_effect(p, &r.ct, NULL, &r.effect, YGLC_EFFECT_SMOOTH,
                              &r.duration_ms, 500) != 0) {
        return rpc_fail(p, YGLC_ERR_INVALID);
    }
    return rpc_post_or_fail(p, YGLC_REQ_SET_CT, &r, sizeof r);
}

/* 0x100bd560 yglc_json_parse_cf: [count, action, "flow"] -> "count,action,flow".
 * BUG: on failure it already replies AND frees the parser, but both callers
 *      (start_cf, set_scene "cf") use the parser again -> double reply +
 *      use-after-free. Here it only reports failure; callers reply once. */
static int rpc_parse_cf(yglc_rpc_parser_t *p, yglc_cf_req_t *cf)
{
    char flow[0x204];
    cf->start_opt = 1;
    if (yglc_rpc_pop_int(p, &cf->count) == 0 && yglc_rpc_pop_int(p, &cf->action) == 0) {
        memset(flow, 0, 0x200);
        if (yglc_rpc_pop_str(p, flow, 0x200) == 0) {
            snprintf(cf->expr, 0x100, "%d,%d,%s", (int)cf->count, (int)cf->action, flow);
            return 0;
        }
    }
    LOGE("[E] failed to parse flow request - %d\r\n (%s,%d)", 1, "yglc_json_parse_cf", 0x5fa);
    return YGLC_ERR_INVALID;
}

/* 0x100bd610 start_cf [count, action, "flow"] -> req 0x0a */
int yglc_m_start_cf(yglc_rpc_parser_t *p)
{
    yglc_cf_req_t cf;
    memset(&cf, 0, sizeof cf);
    cf.flags = 1;
    cf.enable = 1;
    if (rpc_parse_cf(p, &cf) != 0) {
        return rpc_fail(p, YGLC_ERR_INVALID);
    }
    return rpc_post_or_fail(p, YGLC_REQ_START_CF, &cf, sizeof cf);
}

/* 0x100bbd98 stop_cf [] -> req 0x0b (cf payload with flags=1) */
int yglc_m_stop_cf(yglc_rpc_parser_t *p)
{
    yglc_cf_req_t cf;
    memset(&cf, 0, sizeof cf);
    cf.flags = 1;
    int r = json_rpc_to_yglc(YGLC_REQ_STOP_CF, &cf, sizeof cf, yglc_rpc_on_request_done, p);
    return (r == 0) ? 0 : rpc_fail(p, r);
}

/* 0x100bd668 set_scene ["class", ...]
 *   "color", rgb, bright[, effect/duration]      -> type 1
 *   "ct", ct, bright[, ...]                       -> type 2
 *   "hsv", hue, sat, bright[, ...]                -> type 3
 *   "cf", count, action, "flow"                   -> type 4
 *   "auto_delay_off", bright, minutes             -> type 6
 *   "bright", bright[, ...]                       -> type 8 (0 = power off!)
 *   "nightlight", bright 1..100                   -> type 7 = set_ps("nightlight","on|%03d")
 *   "off" / "keep" / "on"                         -> type 10 / 11 / 12
 * BUG: any parse error still posts the half-filled request (e.g. unknown
 *      class -> type 0 -> bfa60 sets settings->power = ON and returns 1). */
int yglc_m_set_scene(yglc_rpc_parser_t *p)
{
    yglc_scene_req_t s;
    char cls[16];

    memset(&s, 0, sizeof s);
    memset(cls, 0, sizeof cls);
    if (yglc_rpc_pop_str(p, cls, 0x10) != 0) {
        goto post;
    }
    if (strncmp(cls, "color", 6) == 0) {
        s.type = YGLC_SCENE_COLOR;
        s.u.color.flags = 1;
        (void)rpc_pop_values_effect_any(p, (int32_t *)&s.u.color.rgb, &s.u.color.bright,
                                        &s.u.color.effect, YGLC_EFFECT_SMOOTH, &s.u.color.duration, 500);
    } else if (strncmp(cls, "ct", 3) == 0) {
        s.type = YGLC_SCENE_CT;
        if (rpc_pop_values_effect_any(p, &s.u.ct.ct, &s.u.ct.bright, &s.u.ct.effect,
                                      YGLC_EFFECT_SMOOTH, &s.u.ct.duration, 500) == 0) {
            s.u.ct.flags = 1;
        }
    } else if (strncmp(cls, "hsv", 4) == 0) {
        s.type = YGLC_SCENE_HSV;
        if (yglc_rpc_pop_int(p, &s.u.hsv.hue) == 0 && yglc_rpc_pop_int(p, &s.u.hsv.sat) == 0 &&
            yglc_rpc_pop_int(p, &s.u.hsv.bright) == 0 &&
            rpc_parse_duration_effect_any(p, &s.u.hsv.effect, YGLC_EFFECT_SMOOTH,
                                          &s.u.hsv.duration, 500) == 0) {
            s.u.hsv.flags = 1;
        }
    } else if (strncmp(cls, "cf", 3) == 0) {
        s.type = YGLC_SCENE_CF;
        s.u.cf.flags = 1;
        (void)rpc_parse_cf(p, &s.u.cf);
    } else if (strncmp(cls, "auto_delay_off", 0xf) == 0) {
        s.type = YGLC_SCENE_DELAY_OFF;
        (void)rpc_pop_values_effect_any(p, &s.u.delay_off.bright, &s.u.delay_off.minutes,
                                        NULL, 0, NULL, 0);
    } else if (strncmp(cls, "bright", 7) == 0) {
        uint8_t eff = 0;
        s.type = YGLC_SCENE_BRIGHT;
        if (rpc_pop_values_effect_any(p, &s.u.bright.bright, NULL, &eff, YGLC_EFFECT_SMOOTH,
                                      &s.u.bright.duration, 500) == 0) {
            s.u.bright.flags = 1;
        }
        s.u.bright.effect_or_onoff_flags = eff;
    } else if (strncmp(cls, "nightlight", 0xb) == 0) {
        int32_t br = 0;
        if (yglc_rpc_pop_int(p, &br) == 0 && (uint32_t)(br - 1) < 100u) {
            s.type = YGLC_SCENE_SET_PS;
            memcpy(s.u.ps.key, "nightlight", strlen("nightlight") + 1);
            sprintf(s.u.ps.value, "on|%03d", (int)br);
        }
    } else if (strncmp(cls, "off", 4) == 0) {
        s.type = YGLC_SCENE_OFF;
        s.u.bright.effect_or_onoff_flags = 1;
    } else if (strncmp(cls, "keep", 5) == 0) {
        s.type = YGLC_SCENE_KEEP;
    } else if (strncmp(cls, "on", 3) == 0) {
        s.type = YGLC_SCENE_ON;
        s.u.bright.effect_or_onoff_flags = 1;
    }
post:
    {
        int r = json_rpc_to_yglc(YGLC_REQ_SET_SCENE, &s, sizeof s, yglc_rpc_on_request_done, p);
        return (r == 0) ? 0 : rpc_fail(p, r);
    }
}

/* 0x100bc8ac cron_add [type 0..4, minutes > 0, (extra for type 2..4)]
 * BUG: type > 4 answers "ok" without doing anything. */
int yglc_m_cron_add(yglc_rpc_parser_t *p)
{
    yglc_cron_req_t c;
    int32_t v = 0;
    memset(&c, 0, sizeof c);
    if (yglc_rpc_pop_int(p, &v) != 0) {
        return rpc_fail(p, YGLC_ERR_INVALID);
    }
    c.type = (uint8_t)v;
    if (c.type > 4u) {
        return rpc_fail(p, YGLC_OK);
    }
    if (yglc_rpc_pop_int(p, &c.minutes) != 0 || c.minutes <= 0) {
        return rpc_fail(p, YGLC_ERR_INVALID);
    }
    if ((uint32_t)(c.type - 2u) <= 2u && yglc_rpc_pop_int(p, (int32_t *)&c.extra) != 0) {
        return rpc_fail(p, YGLC_ERR_INVALID);
    }
    if (json_rpc_to_yglc(YGLC_REQ_CRON_ADD, &c, sizeof c, yglc_rpc_on_request_done, p) != 0) {
        return rpc_fail(p, YGLC_ERR_NOMEM);
    }
    return 0;
}

/* 0x100bcd14 set_adjust ["increase"|"decrease"|"circle"|"auto", "bright"|"ct"|"color", (opt<3)]
 * Note: duration and value are 0 -> step-table moves with no transition. */
int yglc_m_set_adjust(yglc_rpc_parser_t *p)
{
    yglc_adjust_req_t a;
    char act[16], prop[16];
    int32_t opt = 0;

    memset(act, 0, sizeof act);
    memset(prop, 0, sizeof prop);
    memset(&a, 0, sizeof a);
    a.flags = 1;
    if (yglc_rpc_pop_str(p, act, 0x10) != 0) return rpc_fail(p, YGLC_ERR_INVALID);
    if      (strncmp(act, "increase", 9) == 0) a.action = YGLC_ADJ_INCREASE;
    else if (strncmp(act, "decrease", 9) == 0) a.action = YGLC_ADJ_DECREASE;
    else if (strncmp(act, "circle", 7) == 0)   a.action = YGLC_ADJ_CIRCLE;
    else if (strncmp(act, "auto", 5) == 0)     a.action = YGLC_ADJ_AUTO;
    else return rpc_fail(p, YGLC_ERR_INVALID);

    if (yglc_rpc_pop_str(p, prop, 0x10) != 0) return rpc_fail(p, YGLC_ERR_INVALID);
    if      (strncmp(prop, "bright", 7) == 0) a.prop = YGLC_ADJ_PROP_BRIGHT;
    else if (strncmp(prop, "ct", 3) == 0)     a.prop = YGLC_ADJ_PROP_CT;
    else if (strncmp(prop, "color", 6) == 0)  a.prop = YGLC_ADJ_PROP_COLOR;
    else return rpc_fail(p, YGLC_ERR_INVALID);

    if (yglc_rpc_pop_int(p, &opt) != 0) opt = 0;
    if ((uint8_t)opt >= 3u) return rpc_fail(p, YGLC_ERR_INVALID);
    a.opt = (uint8_t)opt;
    return rpc_post_or_fail(p, YGLC_REQ_ADJUST, &a, sizeof a);
}

/* 0x100bcaa0: [percentage -100..100 (!=0), duration <= 60000, (opt)] */
static int rpc_parse_adjust_pct(yglc_rpc_parser_t *p, yglc_adjust_req_t *a)
{
    int32_t opt = 0;
    if (yglc_rpc_pop_int(p, &a->value) != 0 || a->value == 0 ||
        (uint32_t)(a->value + 100) > 200u) {
        return YGLC_ERR_INVALID;
    }
    if (yglc_rpc_pop_int(p, (int32_t *)&a->duration_ms) != 0 || a->duration_ms > 60000u) {
        return YGLC_ERR_INVALID;
    }
    (void)yglc_rpc_pop_int(p, &opt);
    if (a->value > 0) {
        a->action = YGLC_ADJ_PCT_UP;
    } else {
        a->action = YGLC_ADJ_PCT_DOWN;
        a->value = -a->value;
    }
    a->opt = (uint8_t)opt;
    return 0;
}

/* 0x100bcb00 adjust_bright [pct, duration] */
int yglc_m_adjust_bright(yglc_rpc_parser_t *p)
{
    yglc_adjust_req_t a;
    memset(&a, 0, sizeof a);
    a.flags = 1;
    int r = rpc_parse_adjust_pct(p, &a);
    if (r != 0) return rpc_fail(p, r);
    a.prop = YGLC_ADJ_PROP_BRIGHT;
    return rpc_post_or_fail(p, YGLC_REQ_ADJUST, &a, sizeof a);
}

/* 0x100bcb58 adjust_ct [pct, duration] */
int yglc_m_adjust_ct(yglc_rpc_parser_t *p)
{
    yglc_adjust_req_t a;
    memset(&a, 0, sizeof a);
    a.flags = 1;
    int r = rpc_parse_adjust_pct(p, &a);
    if (r != 0) return rpc_fail(p, r);
    a.prop = YGLC_ADJ_PROP_CT;
    return rpc_post_or_fail(p, YGLC_REQ_ADJUST, &a, sizeof a);
}

/* 0x100bce38 set_ps ["key", "value"|int]  (cloud only - not in LAN whitelist) */
int yglc_m_set_ps(yglc_rpc_parser_t *p)
{
    yglc_ps_req_t ps;
    int32_t iv = 0;
    memset(&ps, 0, sizeof ps);
    if (yglc_rpc_pop_str(p, ps.key, 0x1f) != 0) {
        return rpc_fail(p, YGLC_ERR_INVALID);
    }
    if (yglc_rpc_pop_str(p, ps.value, 0x1ff) != 0 && yglc_rpc_pop_int(p, &iv) == 0) {
        sprintf(ps.value, "%d", (int)iv);
    }
    return rpc_post_or_fail(p, YGLC_REQ_SET_PS, &ps, sizeof ps);
}

/* ===================================================================== */
/*  3. Dispatcher (method table @0x20000aec)                              */
/* ===================================================================== */
static const yglc_rpc_method_t k_yglc_methods[] = {
    { 0, "get_prop",         FUN_100bcc40 },
    { 0, "set_power",        yglc_m_set_power },
    { 0, "set_scene",        yglc_m_set_scene },
    { 0, "set_bright",       yglc_m_set_bright },
    { 0, "set_ct_abx",       yglc_m_set_ct_abx },
    { 0, "set_rgb",          FUN_100bd32c },
    { 0, "set_hsv",          FUN_100bd38c },
    { 0, "start_cf",         yglc_m_start_cf },
    { 0, "stop_cf",          yglc_m_stop_cf },
    { 0, "set_ps",           yglc_m_set_ps },
    { 0, "toggle",           yglc_m_toggle },
    { 0, "adjust_bright",    yglc_m_adjust_bright },
    { 0, "adjust_color",     FUN_100bcbb0 },
    { 0, "adjust_ct",        yglc_m_adjust_ct },
    { 0, "set_adjust",       yglc_m_set_adjust },
    { 0, "set_test",         FUN_100bcff4 },
    { 0, "set_default",      yglc_m_set_default },
    { 0, "set_xy",           FUN_100bd44c },
    { 0, "cron_add",         yglc_m_cron_add },
    { 0, "cron_del",         FUN_100bc930 },
    { 0, "cron_get",         FUN_100bc994 },
    { 0, "set_music",        FUN_100bcf1c },
    { 0, "set_name",         FUN_100bcf98 },
    { 0, "set_indicator",    FUN_100bbc0e },
    { 0, "get_ps",           FUN_100bceb8 },
    { 0, "set_alert",        FUN_100bbc0a },
    { 0, "set_wrgb",         FUN_100bd3ec },
    { 0, "miio_test",        FUN_100bc3f8 },
    { 0, "miio_test_exit",   FUN_100bc9f4 },
    { 0, "get_aging_status", FUN_100bbc14 },
    { 0, "suicide",          FUN_100bbcc0 },
    { 0, "ble_dbg_tbl_dump", FUN_100bc46c },
};

/* 0x100bc7d0: registered for every "user.<method>" in the miIO method
 * registry (0x20001328, 34 x 16 bytes) - same entry point for cloud and LAN.
 * Handlers own the parser: they reply (sync or from yglc_task) and free it.
 * Returns true on error (init failure or unknown method). */
bool yglc_rpc_dispatch(void *rpc_arg, yglc_rpc_reply_fn_t reply, void *reply_arg)
{
    yglc_rpc_parser_t *p = NULL;
    if (yglc_json_parser_init(&p, rpc_arg, reply, reply_arg) != 0) {
        return true;
    }
    LOGI("[I] method %s\n", p->method);
    for (size_t i = 0; i < sizeof k_yglc_methods / sizeof k_yglc_methods[0]; i++) {
        if (strcmp(k_yglc_methods[i].name, p->method) == 0) {
            int r = k_yglc_methods[i].handler(p);
            if (k_yglc_methods[i].auto_reply != 0u) { /* never set in 2.0.2_0048 */
                yglc_rpc_reply_status(r, rpc_arg, reply, reply_arg);
                yglc_rpc_parser_free(p);
            }
            return r != 0;
        }
    }
    /* BUG (original): parser is leaked for unknown method */
    return true;
}

/* call_method (0x100c3814) whitelist for LAN (TCP 55443) clients @0x20000a34:
 * get_prop set_default set_power toggle set_bright set_scene cron_add cron_get
 * cron_del start_cf stop_cf set_ct_abx set_name set_adjust adjust_bright adjust_ct.
 * (set_ps, set_music, adjust_color, set_rgb/hsv are cloud only.) */
bool yglc_lan_method_exposed(const char *method)
{
    for (size_t i = 0; i < 16u && g_lan_exposed_methods[i] != NULL; i++) {
        if (strcmp(g_lan_exposed_methods[i], method) == 0) {
            return true;
        }
    }
    return false;
}

/* ===================================================================== */
/*  4. Request queue                                                      */
/* ===================================================================== */
static inline uint32_t irq_mask_save(void)
{
    uint32_t old, v = 0x80u;
    __asm volatile("mrs %0, basepri" : "=r"(old));
    __asm volatile("msr basepri, %0\n\tisb\n\tdsb" : : "r"(v) : "memory");
    return old;
}

static inline void irq_mask_restore(uint32_t old)
{
    __asm volatile("msr basepri, %0" : : "r"(old) : "memory");
}

static inline bool in_isr(void)
{
    /* original reads ICSR (0xe000ed04) VECTACTIVE byte */
    return (*(volatile uint32_t *)0xe000ed04u & 0xffu) != 0u;
}

/* 0x100bdad8 */
static yglc_req_slot_t *req_slot_alloc(void)
{
    uint32_t key = irq_mask_save();
    for (uint32_t i = 0; i < YGLC_REQ_SLOT_COUNT; i++) {
        if (g_yglc_req_slots[i].in_use == 0u) {
            g_yglc_req_slots[i].in_use = 1u;
            irq_mask_restore(key);
            return &g_yglc_req_slots[i];
        }
    }
    irq_mask_restore(key);
    return NULL;
}

/* 0x100bdb1c */
static void req_slot_free(yglc_req_slot_t *s)
{
    uint32_t key = irq_mask_save();
    s->in_use = 0u;
    irq_mask_restore(key);
}

/* 0x100bdb3c (task and ISR context) */
static int req_queue_send(yglc_req_slot_t *s)
{
    if (!in_isr()) {
        if (xQueueGenericSend(g_yglc_core.queue, &s, 0, 0) != 1) {
            req_slot_free(s);
            return -1;
        }
        return 0;
    }
    int32_t woken = 0;
    if (xQueueGenericSendFromISR(g_yglc_core.queue, &s, &woken, 0) != 0) {
        if (woken != 0) {
            *(volatile uint32_t *)0xe000ed04u = 0x10000000u; /* PENDSVSET */
        }
        /* BUG (original): success from ISR is reported as -1 and failure as 0
         * (the slot is freed on the failure path). */
        return -1;
    }
    req_slot_free(s);
    return 0;
}

/* 0x100bdbc0 */
int yglc_post_request_async(uint8_t id, const void *payload, uint32_t len,
                            yglc_req_done_cb_t cb, void *arg)
{
    yglc_req_slot_t *s = req_slot_alloc();
    if (s == NULL) {
        LOGE("[E] No yglc buffer, req:%d is dropped!\r\n (%s,%d)", id, "yglc_post_request_async", 0x1db);
        return -1;
    }
    s->id = id;
    if (payload != NULL) {
        memcpy(s->payload, payload, len); /* len is never checked against 0x224 */
    }
    s->done_arg = arg;
    s->done_cb = cb;
    s->from_isr = 0;
    return req_queue_send(s);
}

/* ===================================================================== */
/*  5. Leaf request handlers (run in yglc_task)                           */
/* ===================================================================== */

/* 0x100bdcfa: power on with mode -> FUN_100c2574 -> unit_power_on_mode (0x100c243c):
 *   0 normal: only if unit->off: FUN_100c23b0 = NL if (is_night_time || nl_status_cfg==1), else DL
 *   1 CT    : FUN_100c1e62 (select CT channel = DL, apply current ct)
 *   4 CF    : FUN_100c232c(unit, "", 1)   5 NL: FUN_100c2050(unit, 0, dur, save=1)
 *   2,3,>5  : nothing. Always returns 0. */
uint32_t yglc_req_power_on(const yglc_power_req_t *r)
{
    uint32_t dur = (r->effect == YGLC_EFFECT_SUDDEN) ? 0u : r->duration_ms;
    FUN_100c2574(r->flags, r->mode, dur);
    return 0;
}

/* 0x100bdd0e: power off -> FUN_100c1d12(unit, off=1, dur) (all channels fade out) */
uint32_t yglc_req_power_off(const yglc_power_req_t *r)
{
    uint32_t dur = (r->effect == YGLC_EFFECT_SUDDEN) ? 0u : r->duration_ms;
    FUN_100c2484(r->flags, dur);
    return 0;
}

/* 0x100bdd24: toggle by unit->off, fixed smooth 500 ms, mode 0 */
uint32_t yglc_req_toggle(void)
{
    yglc_power_req_t on, off;
    memset(&on, 0, sizeof on);
    on.effect = YGLC_EFFECT_SMOOTH;
    on.duration_ms = 500;
    memset(&off, 0, sizeof off);
    off.effect = YGLC_EFFECT_SMOOTH;
    off.duration_ms = 500;
    if (UNIT()->off == 0) {
        (void)yglc_req_power_off(&off);
    } else {
        (void)yglc_req_power_on(&on);
    }
    return 0;
}

/* 0x100bdd7c: set bright of the ACTIVE channel (DL bright in day mode, NL bright
 * in night mode!) -> FUN_100c2594 -> FUN_100c1d4a(unit, bright, dur, 0)
 * -> yglc_light_channel_set_bright. 0x100bddc4 (req 0x27) passes v2 = 1. */
uint32_t yglc_req_set_bright(const yglc_bright_req_t *r)
{
    LOGI("[I] new bright:%d\r\n", (int)r->bright);
    uint32_t dur = (r->effect == YGLC_EFFECT_SUDDEN) ? 0u : r->duration_ms;
    FUN_100c2594(r->flags, r->bright, dur, 0);
    return 0;
}

static uint32_t req_set_bright2(const yglc_bright_req_t *r) /* 0x100bddc4 */
{
    LOGI("[I] new bright 2:%d\r\n", (int)r->bright);
    uint32_t dur = (r->effect == YGLC_EFFECT_SUDDEN) ? 0u : r->duration_ms;
    FUN_100c2594(r->flags, r->bright, dur, 1);
    return 0;
}

/* 0x100bde0c / 0x100bde20: CT (bright = -1 keep). FUN_100c1e92 first switches
 * to the CT-capable channel => set_ct_abx in NL mode returns to DL mode. */
static uint32_t req_set_ct(const yglc_ct_req_t *r)
{
    uint32_t dur = (r->effect == YGLC_EFFECT_SUDDEN) ? 0u : r->duration_ms;
    FUN_100c2650(r->flags, r->ct, -1, dur);
    return 0;
}

/* 0x100bdcb0: scene "bright" 0 => off and make next power-on full bright */
void yglc_reset_bright_to_max(void)
{
    yglc_unit_view_t *u = UNIT();
    if (u->active_mode == 1) {
        u->nl_br_effective = 100;
    }
    u->ch[u->active]->bright = 100;
    u->active_bright = 100;
}

/* 0x100bdfd8: start flow on the active channel (start_cf / scene cf) */
static uint32_t req_start_cf(const yglc_cf_req_t *cf)
{
    if ((cf->flags & 1u) == 0u) {
        return 0;
    }
    yglc_unit_view_t *u = UNIT();
    uint32_t opt = cf->start_opt;
    if (u->flowing == 0) {
        *((uint8_t *)u + 0x158) = 0; /* unit flow-restore flag */
    }
    FUN_100c1db8(u, YGLC_CMODE_CF, 300);
    return FUN_100c17b8(unit_active_ch(u), cf->expr, opt);
}

/* 0x100bdebc: adjust (set_adjust / adjust_* / BLE +/- / sun). */
uint32_t yglc_req_adjust(const yglc_adjust_req_t *a)
{
    yglc_power_req_t on;
    yglc_unit_view_t *u = UNIT();
    const bool off = (u->off == 1);

    memset(&on, 0, sizeof on);
    on.effect = YGLC_EFFECT_SMOOTH;
    on.duration_ms = S()->trans_dflt_ms;
    if ((a->flags & 1u) == 0u) {
        return 1;
    }
    if (off) {
        if (a->action == YGLC_ADJ_AUTO) {
            /* "auto" while OFF = power on in the property's mode */
            if      (a->prop == YGLC_ADJ_PROP_CT)     on.mode = YGLC_PON_MODE_CT;
            else if (a->prop == YGLC_ADJ_PROP_COLOR)  on.mode = YGLC_PON_MODE_RGB;
            else if (a->prop != YGLC_ADJ_PROP_BRIGHT) return 1;
            (void)yglc_post_request_async(YGLC_REQ_POWER_ON, &on, sizeof on, NULL, NULL);
            return 0;
        }
        if ((uint8_t)(a->prop - 4u) > 2u) {
            return 1;
        }
    }
    switch (a->prop) {
    case YGLC_ADJ_PROP_BRIGHT:
        /* FUN_100c26c8 -> yglc_light_unit_adjust_bright(unit, action, dur, value):
         * PCT_UP/PCT_DOWN on active channel bright (down to 0 => unit OFF),
         * INCREASE/DECREASE/CIRCLE via active channel step table (+0x138). */
        return FUN_100c26c8(a->flags, a->action, a->duration_ms, a->value);
    case YGLC_ADJ_PROP_CT:
        if (a->opt == 1u) {
            if (off) {
                on.mode = YGLC_PON_MODE_CT;
                (void)yglc_post_request_async(YGLC_REQ_POWER_ON, &on, sizeof on, NULL, NULL);
            } else if (u->ch[0]->color_mode != YGLC_CMODE_CT || u->ch[0]->flowing != 0) {
                return 1;
            }
        } else if (off && (uint8_t)(a->action - 4u) > 1u) {
            return 0;
        }
        return FUN_100c2670(a->flags, a->action, a->duration_ms, a->opt, a->value);
    case YGLC_ADJ_PROP_COLOR:
        if (a->opt == 1u) {
            if (off) {
                on.mode = YGLC_PON_MODE_RGB;
                (void)yglc_post_request_async(YGLC_REQ_POWER_ON, &on, sizeof on, NULL, NULL);
            } else if (u->ch[0]->color_mode != YGLC_CMODE_CT || u->ch[0]->flowing != 0) {
                return 1;
            }
        } else if (off) {
            return 0;
        }
        return FUN_100c269c(a->flags, a->action, a->duration_ms, a->value, a->opt);
    default:
        return 1;
    }
}

/* 0x100be058 set_default: only when ON and save_state == 0, copies runtime NL
 * flag into nl_status_cfg and saves psm category 1 (bright, ct, color_mode,
 * nl_status, nl_br). */
uint32_t yglc_req_set_default(void)
{
    yglc_settings_view_t *s = S();
    if (s->power == 1u || s->save_state != 0u) {
        return YGLC_ERR_ILLEGAL;
    }
    s->unit->nl_status_cfg = s->unit->nl_active;
    FUN_100c2ebc(1);
    return 0;
}

/* 0x100bea68: alert blink if enabled in cfg_alert_ctrl mask */
static uint32_t req_alert(const uint32_t *idx)
{
    if (((1u << (*idx & 0xffu)) & S()->alert_mask) == 0u) return 1;
    if (*idx == 0u)      FUN_100bb8e0(500, 10);
    else if (*idx == 1u) FUN_100bb8e0(500, 6);
    return 0;
}

/* 0x100be364 */
uint32_t yglc_set_music(const yglc_music_req_t *m);

/* 0x100bfa60: generic scene apply. NOTE: first marks settings->power = ON. */
uint32_t yglc_scene_apply(const yglc_scene_req_t *sc)
{
    const void *pl = &sc->u;
    S()->power = 0; /* FUN_100be34c(0) */
    switch (sc->type) {
    case YGLC_SCENE_COLOR: {
        uint32_t dur = (sc->u.color.effect == 0) ? 0u : sc->u.color.duration;
        FUN_100c25d4(sc->u.color.flags, sc->u.color.rgb, sc->u.color.bright, dur);
        return 0;
    }
    case YGLC_SCENE_CT: {
        uint32_t dur = (sc->u.ct.effect == 0) ? 0u : sc->u.ct.duration;
        FUN_100c2650(sc->u.ct.flags, sc->u.ct.ct, sc->u.ct.bright, dur);
        return 0;
    }
    case YGLC_SCENE_HSV: {
        uint32_t dur = (sc->u.hsv.effect == 0) ? 0u : sc->u.hsv.duration;
        FUN_100c25fc(sc->u.hsv.flags, sc->u.hsv.hue, sc->u.hsv.sat, sc->u.hsv.bright, dur);
        return 0;
    }
    case YGLC_SCENE_CF:
    case YGLC_SCENE_CF2:
        return req_start_cf(&sc->u.cf);
    case YGLC_SCENE_SET_PS:
        return yglc_set_ps((const yglc_ps_req_t *)pl);
    case YGLC_SCENE_OFF:
        return yglc_req_power_off((const yglc_power_req_t *)pl);
    case YGLC_SCENE_ON:
        return yglc_req_power_on((const yglc_power_req_t *)pl);
    case YGLC_SCENE_BRIGHT:
        return yglc_req_set_bright((const yglc_bright_req_t *)pl);
    case YGLC_SCENE_WRGB: {
        const uint32_t *w = (const uint32_t *)pl;
        FUN_100c25b4(w[0], &w[1], w[2]);
        return 0;
    }
    default:
        return 1;
    }
}

/* Inline part of yglc_proc_request for req 0x10 (0x100bfc94..0x100bfdee). */
static uint32_t proc_scene(const yglc_scene_req_t *sc)
{
    yglc_settings_view_t *s = S();
    const uint32_t trans = s->trans_dflt_ms;
    const uint32_t power_off = s->power;
    yglc_bright_req_t br;
    yglc_power_req_t on, off;
    yglc_cron_req_t cron;

    memset(&cron, 0, sizeof cron);
    memset(&on, 0, sizeof on);
    on.effect = 1;
    on.duration_ms = trans;
    memset(&off, 0, sizeof off);
    off.effect = 1;
    off.duration_ms = trans;
    memset(&br, 0, sizeof br);
    br.effect = 1;
    br.duration_ms = trans;

    switch (sc->type) {
    case YGLC_SCENE_CF:
        break;
    case YGLC_SCENE_DELAY_OFF:
        if ((uint32_t)(sc->u.delay_off.bright + 1) > 101u || sc->u.delay_off.minutes == 0) return 1;
        br.bright = sc->u.delay_off.bright;
        (void)yglc_req_set_bright(&br);
        cron.type = YGLC_CRON_DELAY_OFF;
        cron.minutes = sc->u.delay_off.minutes;
        return FUN_100c00f0(&cron) & 0xffu;
    case YGLC_SCENE_COLOR:
        if ((sc->u.color.rgb - 1u) > 0xfffffeu || (uint32_t)(sc->u.color.bright + 1) > 101u) {
            return 1; /* BUG (original): returns the rgb-1 value instead of 1 for bad rgb */
        }
        break;
    case YGLC_SCENE_CT:
        if ((uint32_t)(sc->u.ct.bright + 1) > 101u) return 1;
        break;
    case YGLC_SCENE_HSV:
        if ((uint32_t)(sc->u.hsv.hue + 1) > 360u || (uint32_t)(sc->u.hsv.sat + 1) > 101u ||
            (uint32_t)(sc->u.hsv.bright + 1) > 101u) return 1;
        break;
    case YGLC_SCENE_BRIGHT: {
        const int32_t b = sc->u.bright.bright;
        if (b > 100) return 1;
        br.flags = sc->u.bright.flags;
        br.bright = b;
        br.effect = (uint8_t)sc->u.bright.effect_or_onoff_flags;
        br.duration_ms = sc->u.bright.duration;
        if (power_off == 0u) {                    /* light is ON */
            if (b == 0) {                         /* bright 0 => OFF, next ON at 100% */
                off.effect = (uint8_t)sc->u.bright.effect_or_onoff_flags;
                off.duration_ms = sc->u.bright.duration;
                (void)yglc_req_power_off(&off);
                yglc_reset_bright_to_max();
                return 0;
            }
            (void)yglc_req_set_bright(&br);
            return 0;
        }
        if (b == 0) return 0;                     /* OFF and 0 -> nothing */
        (void)yglc_req_set_bright(&br);           /* bypasses the deny table */
        on.effect = (uint8_t)sc->u.bright.effect_or_onoff_flags;
        on.duration_ms = sc->u.bright.duration;
        return yglc_req_power_on(&on);            /* mode 0 -> DL or NL by FUN_100c23b0 */
    }
    case YGLC_SCENE_OFF:
        off.flags = sc->u.bright.effect_or_onoff_flags;
        (void)yglc_req_power_off(&off);
        return 0;
    case YGLC_SCENE_KEEP:
        return 0;
    case YGLC_SCENE_ON:
        on.flags = sc->u.bright.effect_or_onoff_flags;
        (void)yglc_req_power_on(&on);
        return 0;
    default:
        break;
    }
    return yglc_scene_apply(sc);
}

/* 0x100bfad4: executes one request. Deny table @0x20007fc4+8 is indexed by
 * settings->power: while OFF the requests 2,4,5,6,7,0x1c are silently
 * dropped with status 0 (miIO reply "ok"!). */
uint32_t yglc_proc_request(const uint8_t *req, void *out)
{
    yglc_settings_view_t *s = S();
    const uint8_t id = req[0];
    const void *pl = req + 4;

    if (g_yglc_core.deny[s->power & 1u][id % YGLC_REQ_COUNT] != 0u) {
        LOGE("[E] Can not execute req:%d under state:%d\r\n (%s,%d)", id, (int)s->power,
             "yglc_proc_request", 0x247);
        return 0;
    }
    switch (id) {
    case YGLC_REQ_GET_SETTINGS:
        memcpy(out, s, sizeof *s);
        return 0;
    case YGLC_REQ_POWER_ON:   return yglc_req_power_on((const yglc_power_req_t *)pl);
    case YGLC_REQ_POWER_OFF:  return yglc_req_power_off((const yglc_power_req_t *)pl);
    case YGLC_REQ_TOGGLE:     return yglc_req_toggle();
    case YGLC_REQ_SET_BRIGHT: {
        const yglc_bright_req_t *b = (const yglc_bright_req_t *)pl;
        return ((uint32_t)(b->bright - 1) < 100u) ? yglc_req_set_bright(b) : 1u;
    }
    case YGLC_REQ_SET_BRIGHT2: {
        const yglc_bright_req_t *b = (const yglc_bright_req_t *)pl;
        return ((uint32_t)(b->bright - 1) < 100u) ? req_set_bright2(b) : 1u;
    }
    case YGLC_REQ_SET_CT: {
        const yglc_ct_req_t *c = (const yglc_ct_req_t *)pl;
        return (FUN_100c2628(c->flags, c->ct) == 1) ? req_set_ct(c) : 1u;
    }
    case YGLC_REQ_SET_RGB: {
        const yglc_rgb_req_t *c = (const yglc_rgb_req_t *)pl;
        if ((c->rgb - 1u) > 0xfffffeu) return 1;
        FUN_100c25d4(c->flags, c->rgb, -1, (c->effect == 0) ? 0u : c->duration_ms);
        return 0;
    }
    case YGLC_REQ_SET_HSV: {
        const yglc_hsv_req_t *h = (const yglc_hsv_req_t *)pl;
        if ((uint32_t)(h->hue + 1) > 0x168u || (uint32_t)(h->sat + 1) > 0x65u) return 1;
        FUN_100c25fc(h->flags, h->hue, h->sat, -1, (h->effect == 0) ? 0u : h->duration_ms);
        return 0;
    }
    case YGLC_REQ_SET_WRGB: {
        const uint32_t *w = (const uint32_t *)pl;
        FUN_100c25b4(w[0], &w[1], w[2]);
        return 0;
    }
    case YGLC_REQ_START_CF:   return req_start_cf((const yglc_cf_req_t *)pl);
    case YGLC_REQ_STOP_CF:
        if ((((const yglc_cf_req_t *)pl)->flags & 1u) != 0u) (void)FUN_100c1d64(UNIT());
        return 0;
    case YGLC_REQ_CF_UPDATE:
        FUN_100c2366(UNIT());
        return 0;
    case YGLC_REQ_STUB_0C: case YGLC_REQ_STUB_0D: case YGLC_REQ_STUB_0E: case YGLC_REQ_STUB_0F:
    case YGLC_REQ_STUB_11: case YGLC_REQ_STUB_12: case YGLC_REQ_STUB_13: case YGLC_REQ_STUB_18:
    case YGLC_REQ_GET_PS:     case YGLC_REQ_NAME_CHANGED:
        return 0;
    case YGLC_REQ_SET_SCENE:  return proc_scene((const yglc_scene_req_t *)pl);
    case YGLC_REQ_SET_DEFAULT: return yglc_req_set_default();
    case YGLC_REQ_CRON_ADD: {
        const yglc_cron_req_t *c = (const yglc_cron_req_t *)pl;
        if (c->minutes <= 0) return YGLC_ERR_ILLEGAL;
        if (c->type == YGLC_CRON_DELAY_OFF && s->power == 1u) return YGLC_ERR_ILLEGAL;
        if (c->type == YGLC_CRON_TYPE1 && s->power == 0u) return YGLC_ERR_ILLEGAL;
        if (c->type > 1u) return YGLC_ERR_ILLEGAL;
        return FUN_100c00f0(c) & 0xffu;
    }
    case YGLC_REQ_CRON_DEL: {
        const yglc_cron_sel_req_t *c = (const yglc_cron_sel_req_t *)pl;
        uint32_t r = 0;
        for (int32_t i = 0; i < c->count; i++) r = FUN_100c0158(c->types[i]) & 0xffu;
        return r;
    }
    case YGLC_REQ_CRON_GET:   return FUN_100c0194((const yglc_cron_sel_req_t *)pl, out) & 0xffu;
    case YGLC_REQ_SET_PS:     return yglc_set_ps((const yglc_ps_req_t *)pl);
    case YGLC_REQ_ADJUST:     return yglc_req_adjust((const yglc_adjust_req_t *)pl);
    case YGLC_REQ_ALERT:      return req_alert((const uint32_t *)pl);
    case YGLC_REQ_SET_MUSIC:  return yglc_set_music((const yglc_music_req_t *)pl);
    case YGLC_REQ_REPORT_ALL: FUN_100c27b8(1); return 0;
    case YGLC_REQ_SET_TEST:   return FUN_100c26e8((const yglc_test_req_t *)pl);
    case YGLC_REQ_LAN_CTRL_INIT:
        LOGI("[I] enter lan-ctrl init\r\n");
        if (s->lan_ctrl == 0u) return 0;
        lan_ctrl_init();
        FUN_100c45a6();
        return 0;
    default: /* 0x08 set_xy, 0x20, 0x22..0x25 */
        LOGE("[E] Unsupported request %d, drop it.\r\n (%s,%d)", id, "yglc_proc_request", 0x390);
        return YGLC_ERR_GENERAL;
    }
}

/* 0x100bff30 "yglc_task" (stack 0x1000). Single consumer of the queue; also
 * drives the yglc software timers (cron, transitions, propsync, monitor). */
void yglc_task(void *arg)
{
    (void)arg;
    uint32_t next_flush = arch_os_ms_now() + 1000u;
    bool wrapped = false;
    uint8_t out[0x118];

    for (;;) {
        uint64_t next = 0;
        uint32_t wait = (FUN_100c35f2(g_yglc_timer_ctl, &next) < 0) ? 0xffffffffu : (uint32_t)next;
        yglc_req_slot_t *slot = NULL;
        const int got = (xQueueGenericReceive(g_yglc_core.queue, &slot, wait, 0) == 1) ? 0 : -1;

        yglc_timer_clock_init(g_yglc_timer_ctl);
        FUN_100c35b4(g_yglc_timer_ctl);            /* run expired yglc timers */
        if (got == 0) {
            memset(out, 0, sizeof out);
            uint32_t st = yglc_proc_request(&slot->id, out);
            if (slot->done_cb != NULL) {
                slot->done_cb(st, out, slot->done_arg);
            }
            req_slot_free(slot);
        }
        const uint32_t now = arch_os_ms_now();
        if ((!wrapped && next_flush <= now) || got == 0) {
            /* report changed props (cloud "props" + LAN notify), save psm cat 4, nv cache */
            FUN_100c30b8(1);
            wrapped = now > 0xffffff37u; /* BUG: after wrap only requests trigger flushes */
            next_flush = now + 200u;
        }
    }
}

/* ===================================================================== */
/*  6. set_ps dispatcher, nighttime, night light                          */
/* ===================================================================== */

/* 0x100be088 cfg_lan_ctrl 0|1 */
static uint32_t ps_lan_ctrl(const char *v)
{
    uint32_t x;
    if (sscanf(v, "%d", &x) != 1 || x >= 2u) return 1;
    if (x != S()->lan_ctrl) {
        S()->lan_ctrl = x;
        if (x == 0u) {
            FUN_100c458c();
            return 0;
        }
        lan_ctrl_init();
        FUN_100c45a6();
    }
    return 0;
}

/* 0x100be0d0 miband_sleep 0|1 */
static uint32_t ps_miband_sleep(const char *v)
{
    uint32_t x;
    if (sscanf(v, "%d", &x) != 1 || x >= 2u) return 1;
    S()->miband_sleep = x;
    return 0;
}

/* 0x100be10c */
static void nighttime_persist(const char *s)
{
    char cur[0x24];
    memset(cur, 0, sizeof cur);
    if (psm_get_variable("app_data", "nighttime", cur, 0x23) != 0 || strcmp(s, cur) != 0) {
        psm_set_variable("app_data", "nighttime", s);
    }
}

/* 0x100be150 "nighttime": "enable|HHMMSS|HHMMSS|MM", "disable|...", "set|...".
 * MM = auto delay-off minutes applied when the light is powered on at night.
 * NOTE: range checks allow minute/second 60 (<= 0x3c). */
uint32_t yglc_ps_nighttime(const char *value)
{
    char buf[0x24];
    char *save = NULL;
    uint32_t h1 = 0, m1 = 0, s1 = 0, h2 = 0, m2 = 0, s2 = 0, mins = 0;
    int32_t start = -1, end = -1;
    yglc_settings_view_t *st = S();

    memset(buf, 0, sizeof buf);
    memcpy(buf, value, strlen(value)); /* BUG: no bound check against 0x24 */
    const char *op = strtok_r(buf, "|", &save);
    const char *t = strtok_r(NULL, "|", &save);
    if (t != NULL) {
        if (sscanf(t, "%02d%02d%02d", &h1, &m1, &s1) != 3 || h1 > 23u || m1 > 60u || s1 > 60u) return 1;
        if ((t = strtok_r(NULL, "|", &save)) == NULL) return 1;
        if (sscanf(t, "%02d%02d%02d", &h2, &m2, &s2) != 3 || h2 > 23u || m2 > 60u || s2 > 60u) return 1;
        if ((t = strtok_r(NULL, "|", &save)) == NULL) return 1;
        if (sscanf(t, "%02d", &mins) != 1 || mins > 60u) return 1;
        start = (int32_t)((h1 * 60u + m1) * 60u + s1);
        end = (int32_t)((h2 * 60u + m2) * 60u + s2);
    }
    if (op == NULL) return 1;
    bool store = false;
    if (strncmp(op, "disable", strlen("disable")) == 0) {
        st->night_enabled = 0;
        store = (start != -1 && end != -1);
    } else if (strncmp(op, "enable", strlen("enable")) == 0) {
        st->night_enabled = 1;
        store = (start != -1 && end != -1);
    } else if (strncmp(op, "set", strlen("set")) == 0 && start != -1 && end != -1) {
        store = true;
    } else {
        return 1;
    }
    if (store) {
        st->night_start_s = start;
        st->night_end_s = end;
        st->night_delayoff_min = (int32_t)mins;
        if (mins != 0u) {
            st->delayoff_dflt_min = (int32_t)mins;
        }
    }
    sprintf(st->nighttime, st->night_enabled ? "enable|%02d%02d%02d|%02d%02d%02d|%02d"
                                             : "disable|%02d%02d%02d|%02d%02d%02d|%02d",
            (int)(st->night_start_s / 3600), (int)((st->night_start_s % 3600) / 60),
            (int)(st->night_start_s % 60), (int)(st->night_end_s / 3600),
            (int)((st->night_end_s % 3600) / 60), (int)(st->night_end_s % 60),
            (int)st->night_delayoff_min);
    nighttime_persist(st->nighttime);
    return 0;
}

/* 0x100be560: true when nighttime is enabled and local time is inside
 * [start, end] (wrapping over midnight). Without NTP returns the cached
 * settings->is_night_time (0 after a cold boot - not restored). Updates
 * settings->is_night_time (prop "is_night_time", not reported). */
bool yglc_is_night_time(void)
{
    yglc_settings_view_t *s = S();
    const int32_t start = s->night_start_s;
    const int32_t end = s->night_end_s;

    LOGI("[I] nighttime enabled : %d\r\n", s->night_enabled);
    if (s->night_enabled == 0u) {
        s->is_night_time = 0;
        return false;
    }
    if (g_ntp_time_valid == 0u) {
        LOGI("[I] time info not received from ntp server yet\r\n");
        return s->is_night_time != 0u;
    }
    int32_t t = (int32_t)FUN_100f715a(g_miio_net);
    if ((uint32_t)(t + 0xa8c0) > 0x15180u) { /* |gmt offset| > 12h -> default +8h */
        t = 0x7080;
    }
    t += FUN_100a8650();
    int32_t tm[3] = { 0, 0, 0 };   /* sec, min, hour */
    FUN_100a8fd4(&t, tm);
    const int32_t now = (tm[2] * 60 + tm[1]) * 60 + tm[0];
    LOGI("[I] night = %d->%d, now= %d\r\n", (int)start, (int)end, (int)now);

    bool night;
    if (start < end) {
        night = (start <= now) && (now <= end);
    } else {
        night = (start <= now) || (now <= end);
    }
    s->is_night_time = night ? 1u : 0u;
    return night;
}

/* 0x100be44c set_ps "nightlight": "off" | "on|NNN" | "set|NNN" (NNN 0..100, 0 = keep)
 *   off  -> yglc_light_unit_disable_nl(unit, trans, save=1, 1)  (back to DL, nl_status_cfg=0)
 *   on   -> FUN_100c2050(unit, NNN, trans, save=1)               (enter NL, nl_status_cfg=1)
 *   set  -> FUN_100c2008(unit, NNN, trans, activate=0)           (NL bright, no mode switch) */
uint32_t yglc_ps_nightlight(const char *value)
{
    char buf[0x200];
    char *save = NULL;
    yglc_settings_view_t *s = S();

    memcpy(buf, value, sizeof buf);
    const char *op = strtok_r(buf, "|", &save);
    if (op == NULL) return 0;
    if (strncmp(op, "off", strlen("off")) == 0) {
        yglc_light_unit_disable_nl(s->unit, s->trans_dflt_ms, 1, 1);
        return 0;
    }
    const char *arg = strtok_r(NULL, "|", &save);
    uint32_t br = 0;
    if (arg != NULL) {
        br = (uint32_t)atoi(arg);
        if (br > 100u) return 1;
    }
    if (strncmp(op, "on", strlen("on")) == 0) {
        (void)FUN_100c2050(s->unit, br, s->trans_dflt_ms, 1);
    } else if (strncmp(op, "set", strlen("set")) == 0) {
        (void)FUN_100c2008(s->unit, br, s->trans_dflt_ms, 0);
    }
    return 0;
}

/* 0x100bf740 set_ps "pseudo_beacon": "eid|code" -> virtual BLE event
 * (e.g. "4097|4" = moon key click). Useful for testing remote logic. */
uint32_t yglc_ps_pseudo_beacon(const char *value)
{
    uint32_t eid = 0, code = 0;
    uint8_t mac[6] = { 0xff, 0, 0, 0, 0, 0 };
    if ((uint8_t)sscanf(value, "%d|%d", &eid, &code) != 2u) return 1;
    uint8_t edata[4] = { (uint8_t)code, (uint8_t)(code >> 8), (uint8_t)(code >> 16), (uint8_t)(code >> 24) };
    return yglc_handle_ble_event(mac, eid & 0xffffu, 3, edata);
}

/* 0x100bf77c set_ps dispatcher (Ghidra name "yglc_set_trans_default"). */
uint32_t yglc_set_ps(const yglc_ps_req_t *ps)
{
    yglc_settings_view_t *s = S();
    const char *v = ps->value;
    uint32_t a = 0, b = 0;

    if (strcmp(ps->key, "cfg_save_state") == 0) {
        if ((uint8_t)sscanf(v, "%d", &a) != 1u || a > 1u) return 1;
        if (a != s->save_state) {
            s->save_state = a;
            FUN_100c2eb0();
        }
        return 0;
    }
    if (strcmp(ps->key, "cfg_lan_ctrl") == 0)  return ps_lan_ctrl(v);
    if (strcmp(ps->key, "nighttime") == 0)     return yglc_ps_nighttime(v);
    if (strcmp(ps->key, "nightlight") == 0)    return yglc_ps_nightlight(v);
    if (strcmp(ps->key, "miband_sleep") == 0)  return ps_miband_sleep(v);
    if (strcmp(ps->key, "pseudo_beacon") == 0) return yglc_ps_pseudo_beacon(v);
    if (strcmp(ps->key, "cfg_alert_ctrl") == 0) {
        if ((uint8_t)sscanf(v, "%d|%d", &a, &b) != 2u || a > 31u || b > 1u) return 1;
        if (((s->alert_mask >> a) & 1u) != b) s->alert_mask ^= (1u << a);
        return 0;
    }
    if (strcmp(ps->key, "sub_model") == 0) {
        (void)sscanf(v, "%d", &b);
        if (b >= 1000u) {
            LOGE("[E] Invalid sub_model configuration value\r\n (%s,%d)", "yglc_set_ps_sub_model", 0x1274);
            return 1;
        }
        if (b != s->sub_model) s->sub_model = b;
        else LOGI("[I] No change to sub_model, ignore\r\n");
        return 0;
    }
    if (strcmp(ps->key, "cfg_init_power") == 0) {
        /* 0 = restore saved power (needs save_state), 1 = always ON, 2 = stay OFF */
        if (sscanf(v, "%d", &a) != 1 || a > 2u) return 1;
        s->init_power_opt = a;
        return 0;
    }
    if (strcmp(ps->key, "trans_default") == 0) {
        /* "interval_ms,power_on_effect" interval <= 3600000, effect < 5 */
        const char *comma = NULL;
        for (const char *q = v; *q != '\0'; q++) {
            if (*q == ',') { comma = q; break; }
        }
        if (comma == NULL) {
            LOGE("[E] trans default setted by server were invalid, drop it\r\n (%s,%d)",
                 "yglc_set_trans_default", 0x13cf);
            return 1;
        }
        long iv = strtol(v, NULL, 10);
        if (iv < 0 || (uint32_t)iv > 3600000u) return 1;
        s->trans_dflt_ms = (uint32_t)iv;
        long eff = strtol(comma + 1, NULL, 10);
        if (eff < 0 || eff >= 5) return 1;
        s->power_on_effect = (uint8_t)eff;
        return 0;
    }
    return FUN_100bb8dc(ps);
}

/* 0x100bf1e0: helper posting set_ps("nightlight", op) - used by BLE keys. */
uint32_t yglc_post_nightlight_ps(int op, uint32_t bright)
{
    yglc_ps_req_t ps;
    strncpy(ps.key, "nightlight", strlen("nightlight") + 1);
    if (op == 1)      sprintf(ps.value, "on|%03d", (int)bright);
    else if (op == 0) sprintf(ps.value, "off");
    else if (op == 2) sprintf(ps.value, "set|%03d", (int)bright);
    (void)yglc_post_request_async(YGLC_REQ_SET_PS, &ps, sizeof ps, NULL, NULL);
    return 0;
}

/* ===================================================================== */
/*  7. BLE remote                                                         */
/* ===================================================================== */
static void ble_post_power(uint8_t id, uint32_t flags, uint32_t mode)
{
    yglc_power_req_t r;
    memset(&r, 0, sizeof r);
    r.flags = flags;
    r.effect = YGLC_EFFECT_SMOOTH;
    r.duration_ms = 500;
    r.mode = mode;
    (void)yglc_post_request_async(id, &r, sizeof r, NULL, NULL);
}

static void ble_on(uint32_t f)  { ble_post_power(YGLC_REQ_POWER_ON, f, 0); }  /* 0x100be728 */
static void ble_off(uint32_t f) { ble_post_power(YGLC_REQ_POWER_OFF, f, 0); } /* 0x100be752 */

static void ble_toggle(uint32_t f) /* 0x100be77c (by settings->power) */
{
    if (S()->power == 0u) ble_off(f);
    else ble_on(f);
}

/* 0x100be798: CT step via adjust (DL only, not in NL, unit ON). */
static void ble_ct_step(uint32_t flags, int mode)
{
    yglc_settings_view_t *s = S();
    yglc_unit_view_t *u = s->unit;
    if (s->power != 0u || u->nl_active == 1 || (flags & 1u) == 0u || u->off != 0) return;
    yglc_channel_view_t *ch = u->ch[u->active];
    if (ch->ct_steps == NULL) return;
    bool decrease;
    if (mode == 1) {
        ch->ct_dir = 0;
        decrease = true;
    } else if (mode == 0) {
        ch->ct_dir = 1;
        decrease = false;
    } else if (mode == 5) { /* "sun" key: ping-pong 2700 -> 6500 -> 2700 */
        if (ch->ct_steps[ch->ct_steps_n - 1] <= ch->ct) ch->ct_dir = 0;
        if (ch->ct <= ch->ct_steps[0]) ch->ct_dir = 1;
        decrease = (ch->ct_dir != 1);
    } else {
        return;
    }
    yglc_adjust_req_t a;
    memset(&a, 0, sizeof a);
    a.flags = 1;
    a.action = decrease ? YGLC_ADJ_DECREASE : YGLC_ADJ_INCREASE;
    a.prop = YGLC_ADJ_PROP_CT;
    a.duration_ms = s->trans_dflt_ms;
    (void)yglc_post_request_async(YGLC_REQ_ADJUST, &a, sizeof a, NULL, NULL);
}
static void ble_sun(uint32_t f)    { ble_ct_step(f, 5); } /* 0x100be850 */
static void ble_ct_up(uint32_t f)  { ble_ct_step(f, 0); } /* 0x100be85a */
static void ble_ct_dn(uint32_t f)  { ble_ct_step(f, 1); } /* 0x100be864 */

/* 0x100be870 / 0x100be8b0: bright step on ACTIVE channel table (DL [1,10,40,100],
 * NL [1,2,5,10,40,100]); ignored while unit is off. */
static void ble_bright_step(uint32_t flags, uint8_t action)
{
    yglc_settings_view_t *s = S();
    if (s->unit->off != 0) flags &= ~1u;
    if (flags == 0u) return;
    yglc_adjust_req_t a;
    memset(&a, 0, sizeof a);
    a.flags = flags;
    a.action = action;
    a.prop = YGLC_ADJ_PROP_BRIGHT;
    a.duration_ms = s->trans_dflt_ms;
    (void)yglc_post_request_async(YGLC_REQ_ADJUST, &a, sizeof a, NULL, NULL);
}
static void ble_plus(uint32_t f)  { ble_bright_step(f, YGLC_ADJ_INCREASE); }
static void ble_minus(uint32_t f) { ble_bright_step(f, YGLC_ADJ_DECREASE); }

/* 0x100be8f4 / 0x100be944: long press -/+ => first/last entry of active table */
static void ble_bright_edge(uint32_t flags, bool max)
{
    yglc_settings_view_t *s = S();
    yglc_unit_view_t *u = s->unit;
    if (u->off != 0) flags &= ~1u;
    if (flags == 0u || (flags & 1u) == 0u) return;
    yglc_channel_view_t *ch = u->ch[u->active];
    yglc_bright_req_t r;
    r.flags = 0;
    r.bright = max ? ch->bright_steps[ch->bright_steps_n - 1] : ch->bright_steps[0];
    r.effect = YGLC_EFFECT_SMOOTH;
    r._pad09[0] = r._pad09[1] = r._pad09[2] = 0;
    r.duration_ms = s->trans_dflt_ms;
    (void)yglc_post_request_async(YGLC_REQ_SET_BRIGHT, &r, sizeof r, NULL, NULL);
}
static void ble_min(uint32_t f) { ble_bright_edge(f, false); }
static void ble_max(uint32_t f) { ble_bright_edge(f, true); }

/* 0x100be9a0: short notification blink through a CF scene on unit `idx`. */
static void ble_blink_cf(uint32_t idx, uint32_t dur, uint32_t count)
{
    yglc_scene_req_t sc;
    yglc_unit_view_t *u = S()->unit; /* settings->unit[idx], only idx 0 exists */
    yglc_channel_view_t *ch = u->ch[u->active];
    int32_t b = ch->bright;

    memset(&sc, 0, sizeof sc);
    if (ch->flowing == 0 && *((const uint8_t *)ch + 400) == 1u) {
        snprintf(sc.u.cf.expr, 0x100, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", (int)count, 0, (int)dur, 1,
                 0xa8c, 100, (int)dur, 1, 0xa8c, 1);
    } else {
        int32_t dim;
        if (b < 31) {
            dim = (b + 5) * 2;
            if (b < 1) b = 1;
        } else {
            dim = b / 3;
        }
        snprintf(sc.u.cf.expr, 0x100, "%d,%d,%d,%d,0,%d,%d,%d,0,%d", (int)count, 0, (int)dur, 3,
                 (int)dim, (int)dur, 3, (int)b);
    }
    sc.type = YGLC_SCENE_CF;
    sc.u.cf.count = (int32_t)count;
    sc.u.cf.flags = 1u << (idx & 0xffu);
    (void)yglc_post_request_async(YGLC_REQ_SET_SCENE, &sc, sizeof sc, NULL, NULL);
}

/* 0x100beb4c: long press OFF => blink twice + delayed off after delayoff_info min */
static void ble_long_off(uint32_t f)
{
    (void)f;
    yglc_settings_view_t *s = S();
    if (s->power != 0u) return;
    ble_blink_cf(0, 500, 2);
    yglc_cron_req_t c;
    memset(&c, 0, sizeof c);
    c.type = YGLC_CRON_DELAY_OFF;
    c.minutes = s->delayoff_dflt_min;
    (void)yglc_post_request_async(YGLC_REQ_CRON_ADD, &c, sizeof c, NULL, NULL);
}

/* 0x100bf248: moon click = toggle NL (save) while ON; nothing while OFF. */
static void ble_moon(uint32_t f)
{
    (void)f;
    log_printf("===================\n\n");
    yglc_unit_view_t *u = UNIT();
    if (u->off == 1) return;
    if (u->nl_active == 1) (void)yglc_post_nightlight_ps(0, 0);  /* "off" -> DL */
    else                   (void)yglc_post_nightlight_ps(1, 0);  /* "on|000" -> NL, keep nl_br */
}

/* 0x100bf280: moon long press = NL at brightness 1 */
static void ble_moon_long(uint32_t f)
{
    (void)f;
    if (UNIT()->off == 1) return;
    (void)yglc_post_nightlight_ps(1, 1);
}

static void ble_factory_exit(uint32_t f) { ble_evt_test_exit_cbk(f); } /* 0x100beaa0 */

/* Key table @0x200008a4 (YLYK01YL-like remote: 0 on, 1 off, 2 sun, 3 +, 4 moon,
 * 5 -; 8/9 CT+/CT- of another remote = ASSUMPTION). */
static const yglc_ble_key_t k_ble_keys[] = {
    { YGLC_KEY_CODE(0, 0), ble_on,           0 },
    { YGLC_KEY_CODE(0, 1), ble_off,          0 },
    { YGLC_KEY_CODE(0, 3), ble_plus,         1 },
    { YGLC_KEY_CODE(0, 4), ble_moon,         0 },
    { YGLC_KEY_CODE(0, 5), ble_minus,        1 },
    { YGLC_KEY_CODE(0, 2), ble_sun,          1 },
    { YGLC_KEY_CODE(0, 8), ble_ct_up,        1 },
    { YGLC_KEY_CODE(0, 9), ble_ct_dn,        1 },
    { YGLC_KEY_CODE(2, 1), ble_long_off,     1 },
    { YGLC_KEY_CODE(2, 3), ble_max,          1 },
    { YGLC_KEY_CODE(2, 5), ble_min,          1 },
    { YGLC_KEY_CODE(2, 4), ble_moon_long,    0 },
    { 6u,                  ble_factory_exit, 0 }, /* synthetic: factory key sequence */
    { 7u,                  ble_toggle,       0 },
    { 1000u,               ble_off,          1 }, /* synthetic: Mi Band fell asleep */
};

/* 0x100bf29c: seesaw knob. type 3 = click (arg1 = clicks, arg0 = volley),
 * type 4 = rotation (arg1 = bright steps, arg0 = ct steps, signed). */
static int32_t knob_step(const int32_t *tbl, int32_t n, int32_t v, int32_t steps)
{
    for (; steps > 0; steps--) {           /* up: first entry > v */
        int32_t i = 0, nv = tbl[n - 1];
        for (; i < n - 1; i++) {
            if (tbl[i] <= v && v < tbl[i + 1]) { nv = tbl[i + 1]; break; }
        }
        v = nv;
    }
    for (; steps < 0; steps++) {           /* down: last entry < v */
        int32_t i = 0, nv = tbl[0];
        for (; i < n - 1; i++) {
            if (tbl[i] < v && v <= tbl[i + 1]) { nv = tbl[i]; break; }
        }
        v = nv;
    }
    return v;
}

void yglc_seesaw_event(int type, int arg1, int arg0)
{
    yglc_settings_view_t *s = S();
    yglc_unit_view_t *u = s->unit;
    yglc_channel_view_t *ch = (u->nchan < 2) ? u->ch[0] : u->ch[u->active];

    LOGI("[I] seesaw event received : %d - %d - %d\n", type, arg1, arg0);
    if (!(type == 3 && arg0 == 1)) {
        g_seesaw_volley = 0;
    }
    if (type == 3) {
        LOGI("[I] seesaw clicked\n");
        if (arg0 != 0) {
            if (arg0 == 1) log_printf("volley event is not supported yet\n");
            return;
        }
        if (arg1 == 1) {                 /* single click = toggle */
            ble_toggle(0);
        } else if (arg1 == 2) {          /* double click = DL <-> NL */
            if (s->power == 0u && u->nl_active == 1) {
                (void)yglc_post_nightlight_ps(0, 0);
            } else {
                ble_post_power(YGLC_REQ_POWER_ON, 1, YGLC_PON_MODE_NL);
            }
        }
        return;
    }
    if (type != 4) return;
    LOGI("[I] seesaw spinned\n");
    if (s->power != 0u) return;

    int32_t nb = ch->bright, nct = ch->ct;
    bool changed = false;
    if (ch->knob_bright != NULL) {
        nb = knob_step(ch->knob_bright, ch->knob_bright_n, ch->bright, arg1);
        changed = (nb != ch->bright);
    }
    if (ch->knob_ct != NULL) {
        nct = knob_step(ch->knob_ct, ch->knob_ct_n, ch->ct, arg0);
        changed = changed || (nct != ch->ct);
    }
    if (!changed) return;

    yglc_scene_req_t *sc = (yglc_scene_req_t *)pvPortCalloc(1, sizeof(yglc_scene_req_t));
    if (sc == NULL) return;
    if (ch->knob_ct == NULL) {           /* NL channel: scene "bright" */
        sc->type = YGLC_SCENE_BRIGHT;
        sc->u.bright.flags = 1;
        sc->u.bright.bright = nb;
        sc->u.bright.effect_or_onoff_flags = 1;
        sc->u.bright.duration = 400;
        LOGI("[I] seesaw set nl mode bri %d\n", (int)nb);
    } else {                             /* DL channel: scene "ct" */
        sc->type = YGLC_SCENE_CT;
        sc->u.ct.flags = 1;
        sc->u.ct.ct = nct;
        sc->u.ct.effect = 1;
        sc->u.ct.duration = 400;
        sc->u.ct.bright = nb;
        LOGI("[I] seesaw set dl mode bri %d ct %d\n", (int)nb, (int)nct);
    }
    (void)yglc_post_request_async(YGLC_REQ_SET_SCENE, sc, sizeof *sc, NULL, NULL);
    vPortFree(sc);
}

/* 0x100beca4 / 0x100bed54 */
static bool factory_mode(void)
{
    if (g_factory_passed_cache == -1) {
        char tmp[4];
        g_factory_passed_cache = (psm_get_variable("factory", "passed", tmp, 4) == 0) ? 1 : 0;
    }
    return g_factory_passed_cache == 0;
}

/* 0x100bf5a8 */
uint32_t yglc_handle_ble_event(const uint8_t *mac, uint32_t eid, uint32_t len, const uint8_t *edata)
{
    static const uint32_t k_factory_seq[6] = { 1, 3, 5, 3, 5, 1 }; /* @0x1012adb4 */
    yglc_settings_view_t *s = S();
    (void)len;

    if (edata == NULL || mac == NULL) {
        LOGE("[E] invalid edata or mac received.\r\n (%s,%d)", "yglc_handle_ble_event", 0xcf9);
        return 1;
    }
    uint32_t code = edata[2];
    if (eid == YGLC_BLE_EID_SLEEP) {
        /* ASSUMPTION: 1 = asleep, 2 = awake. Transition awake->asleep turns light off. */
        if (s->power == 0u && s->miband_sleep == 1u && g_ble_sleep_prev == 2u && code == 1u) {
            g_ble_sleep_prev = 1;
            code = 1000;
        } else {
            g_ble_sleep_prev = code;
            return 0;
        }
    } else if (eid == YGLC_BLE_EID_BUTTON) {
        code = ((uint32_t)edata[1] << 8) | (code << 16) | edata[0];
        if (factory_mode()) {            /* factory exit sequence off,+,-,+,-,off */
            if (code == k_factory_seq[g_ble_factory_seq_idx]) {
                if (++g_ble_factory_seq_idx > 5u) code = 6;
            } else {
                g_ble_factory_seq_idx = 0;
            }
        }
        /* At night with the light OFF every key except OFF acts as ON
         * (and ON at night -> FUN_100c23b0 selects the night light). */
        if (yglc_is_night_time() && s->power != 0u && (code & 0x1ffffu) != 1u && code != 6u) {
            code = 0;
        }
    } else {
        LOGE("[E] invalid eid=%d received\r\n (%s,%d)", (int)eid, "yglc_handle_ble_event", 0xd3d);
        return 1;
    }

    switch (code >> 16) {
    case YGLC_KEY_TYPE_CLICK:
    case YGLC_KEY_TYPE_LONG:
        for (size_t i = 0; i < sizeof k_ble_keys / sizeof k_ble_keys[0]; i++) {
            if (k_ble_keys[i].code == code) {
                k_ble_keys[i].fn(k_ble_keys[i].arg);
                return 0;
            }
        }
        break;
    case YGLC_KEY_TYPE_SEESAW_CLK:
    case YGLC_KEY_TYPE_SEESAW_ROT:
        yglc_seesaw_event(edata[2], (int8_t)edata[1], (int8_t)edata[0]);
        return 0;
    default:
        break;
    }
    LOGE("[E] event key code %d not supported\r\n (%s,%d)", (int)code, "yglc_handle_ble_event", 0xd65);
    return 0;
}

/* ===================================================================== */
/*  8. Boot / wall-switch power cycle                                     */
/* ===================================================================== */

/* Excerpt of yglc_init_async (0x100bedb4), executed after nv/psm restore
 * (FUN_100c32d0). Only on COLD boot (no warm-boot marker): */
void yglc_boot_decide_power(void)
{
    yglc_settings_view_t *s = S();
    const int32_t fast = yglc_fast_reboot_count_load();
    if (FUN_100c3248() != 1) {
        if (s->init_power_opt == 1u || fast != 0) {
            s->power = 0;                 /* ON (always after a quick re-power) */
        } else if (s->init_power_opt == 2u) {
            s->power = 1;                 /* stay OFF */
        }                                 /* 0: value restored from nv (save_state) */
    }
}

/* 0x100bac8c product init #2: turn the light on if settings->power says ON.
 * Request 1 with mode 0 => FUN_100c23b0: NL if is_night_time() (always false
 * on cold boot - no NTP yet, cache not restored) or nl_status_cfg == 1
 * (restored from nv when save_state=1), else DL. */
uint32_t yglc_product_power_on_at_boot(void)
{
    yglc_settings_view_t *s = S();
    yglc_power_req_t r;
    memset(&r, 0, sizeof r);
    r.effect = YGLC_EFFECT_SMOOTH;
    r.duration_ms = 500;
    if (s->power == 0u) {
        s->power = 1;                     /* reported state follows the unit callback */
        if (s->unit->off == 0) r.flags = 1;
        if (FUN_100c323c() == 2u) r.duration_ms = 300;
        r.mode = YGLC_PON_MODE_NORMAL;    /* <- hook point for "switch cycle -> NL" */
        (void)yglc_post_request_async(YGLC_REQ_POWER_ON, &r, sizeof r, NULL, NULL);
    }
    s->unit->off = 1;
    return 0;
}

/* 0x100bebb8 */
int32_t yglc_fast_reboot_count_load(void)
{
    char buf[8];
    g_fast_reboot_count = (psm_get_variable("app_data", "fast_reboot_count", buf, 8) == 0)
                              ? (int32_t)strtol(buf, NULL, 10) : 0;
    return g_fast_reboot_count;
}

static void fast_reboot_zero_cb(void *t) /* 0x100bd8a8 */
{
    (void)t;
    psm_set_variable("app_data", "fast_reboot_count", "0");
}

/* 0x100becd8: 5 s after boot the counter is cleared. */
static void fast_reboot_count_zero_arm(void)
{
    if (g_fast_reboot_zero_tmr == NULL) {
        arch_os_timer_create(&g_fast_reboot_zero_tmr, "fast_reboot_count_zero", 5000,
                             fast_reboot_zero_cb, NULL, 0, 0);
        return;
    }
    arch_os_timer_activate(g_fast_reboot_zero_tmr);
}

/* 0x100bebf0 yglc_check_fast_reboot(5), called from miio_instance_create
 * (0x100aa9bc) on cold boot of a factory-passed device, after
 * yglc_init_async and FUN_100bed10 (which arms the 5 s zeroing timer).
 * The 5th power-on, each within 5 s of the previous => blink 99x and Wi-Fi
 * reset ("prov-reset-timer" 5 s -> FUN_100bd8c0: cmd.restore + reboot).
 * There is NO "switch cycle => night light" logic in 2.0.2_0048. */
void yglc_check_fast_reboot(int32_t threshold)
{
    fast_reboot_count_zero_arm();     /* done by FUN_100bed10 in firmware */
    const int32_t cnt = yglc_fast_reboot_count_load();
    if (cnt + 1 < threshold) {
        char buf[8];
        memset(buf, 0, sizeof buf);
        sprintf(buf, "%d", (int)(cnt + 1));
        psm_set_variable("app_data", "fast_reboot_count", buf);
        return;
    }
    psm_set_variable("app_data", "fast_reboot_count", "0");
    FUN_100bb8e0(500, 99);
    LOGI("[I] @@@file:[%s] func:[%s] line:[%d] 5 seconds later restet\n", "yglc.c",
         "fast_reboot_check", 0x10cd);
    arch_os_timer_create(&g_prov_reset_tmr, "prov-reset-timer", 5000, FUN_100bd8c0, NULL, 0, 0);
}

/* ===================================================================== */
/*  9. NOT FIRMWARE CODE - proposal: "hybrid" brightness                  */
/* ===================================================================== */
/* Idea: one DL-scale brightness 1..100. Below K the NL LEDs are used with
 * brightness f(bright), at/above K the DL LEDs. To be called from a patched
 * yglc_req_set_bright (0x100bdd7c) - the single choke point of set_bright,
 * scene "bright", auto_delay_off and BLE long +/- - and from the adjust path.
 * Runs in yglc_task context (like the original handler). */
#define HYBRID_K 10
static uint32_t hybrid_nl_level(int32_t b) /* f(): 1..K-1 -> 1..100 (linear) */
{
    return (uint32_t)(1 + ((b - 1) * 99) / (HYBRID_K - 1));
}

uint32_t yglc_hybrid_set_bright(int32_t bright, uint32_t duration_ms)
{
    yglc_unit_view_t *u = UNIT();
    if (bright < 1 || bright > 100) return 1;
    if (bright < HYBRID_K) {
        if (u->active != 1) {
            /* enter NL without touching nl_status_cfg (save = 0) */
            (void)FUN_100c2050(u, hybrid_nl_level(bright), duration_ms, 0);
        } else {
            (void)FUN_100c2008(u, hybrid_nl_level(bright), duration_ms, 0);
        }
        u->ch[0]->bright = bright;  /* keep "bright" prop = requested DL value */
        return 0;
    }
    if (u->active == 1) {
        u->ch[0]->bright = bright;  /* DL comes back directly at the new level */
        yglc_light_unit_disable_nl(u, duration_ms, 0, 0);
        return 0;
    }
    return FUN_100c1d4a(u, bright, duration_ms, 0);
}
