#pragma once
/*
 * Clean C11 reconstruction of the Yeelight Ceiling3 (yeelink.light.ceiling3,
 * 2.0.2_0048, MT7697 Cortex-M4F) light COMMAND layer:
 *
 *   miIO / LAN JSON-RPC  ->  yglc_rpc_dispatch (0x100bc7d0)  ->  method handler
 *   (0x100bc254..0x100bd8c0)  ->  json_rpc_to_yglc (0x100bbd44)
 *   ->  yglc_post_request_async (0x100bdbc0)  ->  queue  ->  yglc_task (0x100bff30)
 *   ->  yglc_proc_request (0x100bfad4)  ->  leaf handlers (0x100bdcfa..0x100be058)
 *   ->  light unit API (0x100c1cb0..0x100c26e8, reconstructed elsewhere).
 *
 * Also: BLE remote / seesaw knob (yglc_handle_ble_event 0x100bf5a8), set_ps
 * dispatcher (0x100bf77c), nighttime (0x100be150/0x100be560), cold-boot power
 * decision (yglc_init_async 0x100bedb4, product init 0x100bac8c) and the
 * "fast reboot" counter (0x100bebb8..0x100bed54, yglc_check_fast_reboot 0x100bebf0).
 *
 * All offsets were verified against the raw image (Thumb-2 disassembly), not
 * only against the Ghidra output (which mis-types several handlers).
 * Comments in Russian, identifiers in English.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== */
/*  Internal request ids (payload byte +4 of a queue slot)                */
/* ===================================================================== */
typedef enum {
    YGLC_REQ_GET_SETTINGS   = 0x00, /* memcpy(out, settings, 0x118)                 */
    YGLC_REQ_POWER_ON       = 0x01, /* yglc_power_req_t   -> FUN_100bdcfa            */
    YGLC_REQ_POWER_OFF      = 0x02, /* yglc_power_req_t   -> FUN_100bdd0e            */
    YGLC_REQ_TOGGLE         = 0x03, /* (no payload)       -> FUN_100bdd24            */
    YGLC_REQ_SET_BRIGHT     = 0x04, /* yglc_bright_req_t  -> FUN_100bdd7c (1..100)   */
    YGLC_REQ_SET_CT         = 0x05, /* yglc_ct_req_t      -> FUN_100bde20            */
    YGLC_REQ_SET_RGB        = 0x06, /* yglc_rgb_req_t     -> FUN_100bde52            */
    YGLC_REQ_SET_HSV        = 0x07, /* yglc_hsv_req_t     -> FUN_100bde76            */
    YGLC_REQ_SET_XY         = 0x08, /* NOT handled -> 4 "general error"              */
    YGLC_REQ_SET_WRGB       = 0x09, /* yglc_rgb_req_t     -> FUN_100bde46            */
    YGLC_REQ_START_CF       = 0x0a, /* yglc_cf_req_t      -> FUN_100bdfd8            */
    YGLC_REQ_STOP_CF        = 0x0b, /* yglc_cf_req_t      -> FUN_100be020            */
    YGLC_REQ_STUB_0C        = 0x0c, /* return 0 */
    YGLC_REQ_STUB_0D        = 0x0d,
    YGLC_REQ_STUB_0E        = 0x0e,
    YGLC_REQ_STUB_0F        = 0x0f,
    YGLC_REQ_SET_SCENE      = 0x10, /* yglc_scene_req_t   -> inline + FUN_100bfa60   */
    YGLC_REQ_STUB_11        = 0x11,
    YGLC_REQ_STUB_12        = 0x12,
    YGLC_REQ_STUB_13        = 0x13,
    YGLC_REQ_SET_DEFAULT    = 0x14, /* -> FUN_100be058                                */
    YGLC_REQ_CRON_ADD       = 0x15, /* yglc_cron_req_t    -> FUN_100c00f0            */
    YGLC_REQ_CRON_DEL       = 0x16, /* {int n; u8 types[4]} -> FUN_100c0158          */
    YGLC_REQ_CRON_GET       = 0x17, /* {int n; u8 types[4]} -> FUN_100c0194          */
    YGLC_REQ_STUB_18        = 0x18,
    YGLC_REQ_SET_PS         = 0x19, /* yglc_ps_req_t      -> yglc_set_ps 0x100bf77c  */
    YGLC_REQ_GET_PS         = 0x1a, /* stub, returns 0                                */
    YGLC_REQ_ADJUST         = 0x1b, /* yglc_adjust_req_t  -> FUN_100bdebc            */
    YGLC_REQ_ALERT          = 0x1c, /* {u32 idx} blink if bit set in alert mask      */
    YGLC_REQ_SET_MUSIC      = 0x1d, /* yglc_music_req_t   -> yglc_set_music 0x100be364 */
    YGLC_REQ_CF_UPDATE      = 0x1e, /* -> FUN_100bdffc -> FUN_100c2366               */
    YGLC_REQ_REPORT_ALL     = 0x1f, /* force full props report FUN_100c27b8(1)       */
    YGLC_REQ_SET_TEST       = 0x21, /* yglc_test_req_t    -> FUN_100c26e8            */
    YGLC_REQ_NAME_CHANGED   = 0x26, /* no-op (props report happens in task loop)     */
    YGLC_REQ_SET_BRIGHT2    = 0x27, /* like 0x04 but channel flag=1; no producer found */
    YGLC_REQ_LAN_CTRL_INIT  = 0x28, /* lan_ctrl_init() if settings->lan_ctrl         */
    YGLC_REQ_COUNT          = 0x29,
} yglc_req_id_t;

/* Status codes returned by yglc_proc_request / handlers and mapped to miIO
 * errors by yglc_rpc_reply_status (0x100bc33c): code -> {"code": -5000-code}. */
typedef enum {
    YGLC_OK               = 0,
    YGLC_ERR_INVALID      = 1, /* "invalid params"   */
    YGLC_ERR_HW           = 2, /* "hardware fault"   */
    YGLC_ERR_TIMEOUT      = 3, /* "timed out"        */
    YGLC_ERR_GENERAL      = 4, /* "general error"    */
    YGLC_ERR_NOMEM        = 5, /* "memory exhausted" */
    YGLC_ERR_ILLEGAL      = 6, /* "illegal request"  */
    YGLC_ERR_INTERNAL     = 7, /* "internal error"   */
} yglc_status_t;

/* Transition effect byte (parsed by FUN_100bd074 / FUN_100bd160). 0 => duration forced to 0. */
typedef enum {
    YGLC_EFFECT_SUDDEN = 0, /* also any unknown string */
    YGLC_EFFECT_SMOOTH = 1,
    YGLC_EFFECT_CURVY  = 2,
    YGLC_EFFECT_LINEAR = 3,
} yglc_effect_t;

/* set_power 4th parameter == light-unit color mode requested at power on
 * (dispatched by FUN_100c243c, light-unit layer). */
typedef enum {
    YGLC_PON_MODE_NORMAL = 0, /* only if unit is OFF: restore DL or NL (FUN_100c23b0) */
    YGLC_PON_MODE_CT     = 1, /* switch to CT-capable channel (DL), keep ct/bright     */
    YGLC_PON_MODE_RGB    = 2, /* unsupported on ceiling3 -> nothing (reply still "ok") */
    YGLC_PON_MODE_HSV    = 3, /* unsupported -> nothing                                */
    YGLC_PON_MODE_CF     = 4, /* start flow with EMPTY expression (0x101171a0 = "")    */
    YGLC_PON_MODE_NL     = 5, /* night light: FUN_100c2050(unit, 0, dur, save=1)       */
} yglc_power_on_mode_t;

/* Channel color modes (bit index in channel->color_mode_mask, +0x178). */
typedef enum {
    YGLC_CMODE_RGB = 1,
    YGLC_CMODE_CT  = 2, /* DL channel mask = 1<<2 */
    YGLC_CMODE_HSV = 3,
    YGLC_CMODE_CF  = 4,
    YGLC_CMODE_NL  = 7, /* NL channel mask = 1<<7 */
} yglc_color_mode_t;

/* set_scene type (yglc_scene_req_t::type). */
typedef enum {
    YGLC_SCENE_NONE       = 0,  /* parse failure: still posted -> bfa60 default -> 1 */
    YGLC_SCENE_COLOR      = 1,
    YGLC_SCENE_CT         = 2,
    YGLC_SCENE_HSV        = 3,
    YGLC_SCENE_CF         = 4,
    YGLC_SCENE_DELAY_OFF  = 6,  /* "auto_delay_off" [bright, minutes] */
    YGLC_SCENE_SET_PS     = 7,  /* "nightlight" -> set_ps("nightlight","on|NNN") */
    YGLC_SCENE_BRIGHT     = 8,
    YGLC_SCENE_CF2        = 9,  /* internal only */
    YGLC_SCENE_OFF        = 10,
    YGLC_SCENE_KEEP       = 11,
    YGLC_SCENE_ON         = 12,
    YGLC_SCENE_WRGB       = 13, /* internal only */
} yglc_scene_type_t;

/* adjust request (0x1b) */
typedef enum {
    YGLC_ADJ_INCREASE = 0, /* next value of step table */
    YGLC_ADJ_DECREASE = 1, /* previous value of step table */
    YGLC_ADJ_PCT_UP   = 2, /* adjust_*: +value %          */
    YGLC_ADJ_PCT_DOWN = 3, /* adjust_*: -value %          */
    YGLC_ADJ_CIRCLE   = 4,
    YGLC_ADJ_AUTO     = 5, /* if OFF: power on in the mode of the property */
} yglc_adjust_action_t;

typedef enum {
    YGLC_ADJ_PROP_BRIGHT = 4,
    YGLC_ADJ_PROP_CT     = 5,
    YGLC_ADJ_PROP_COLOR  = 6,
} yglc_adjust_prop_t;

/* cron types (cron_add first param, 0..4). Only type 0 has an expiry action. */
typedef enum {
    YGLC_CRON_DELAY_OFF = 0, /* power off after N minutes (needs power ON)  */
    YGLC_CRON_TYPE1     = 1, /* accepted only when OFF, no expiry action    */
} yglc_cron_type_t;

/* BLE mi-beacon object ids handled by yglc_handle_ble_event */
#define YGLC_BLE_EID_BUTTON   0x1001u /* remote / seesaw button event       */
#define YGLC_BLE_EID_SLEEP    0x2001u /* Mi Band sleep state (ASSUMPTION)   */

/* Button event code = (edata[2] << 16) | (edata[1] << 8) | edata[0]
 * edata[0] = key index, edata[2] = press type (0 click, 2 long press, 3/4 seesaw). */
#define YGLC_KEY_CODE(type, key) ((((uint32_t)(type)) << 16) | (uint32_t)(key))
#define YGLC_KEY_TYPE_CLICK      0u
#define YGLC_KEY_TYPE_LONG       2u
#define YGLC_KEY_TYPE_SEESAW_CLK 3u
#define YGLC_KEY_TYPE_SEESAW_ROT 4u

/* ===================================================================== */
/*  Request payloads (copied into slot->payload, max 0x224 bytes)         */
/* ===================================================================== */
typedef struct {
    uint32_t flags;        /* +0x00 bit0 = main unit; 0 is treated as 1      */
    uint8_t  effect;       /* +0x04 yglc_effect_t; 0 => duration ignored      */
    uint8_t  _pad05[3];
    uint32_t duration_ms;  /* +0x08 */
    uint32_t mode;         /* +0x0c yglc_power_on_mode_t (power on only)     */
    uint32_t _rsv10;       /* +0x10 */
} yglc_power_req_t;

typedef struct {
    uint32_t flags;        /* +0x00 */
    int32_t  bright;       /* +0x04 1..100 (checked in yglc_proc_request)    */
    uint8_t  effect;       /* +0x08 */
    uint8_t  _pad09[3];
    uint32_t duration_ms;  /* +0x0c */
} yglc_bright_req_t;

typedef struct {
    uint32_t flags;        /* +0x00 */
    int32_t  ct;           /* +0x04 must be inside CT channel [min_ct,max_ct] */
    uint8_t  effect;       /* +0x08 */
    uint8_t  _pad09[3];
    uint32_t duration_ms;  /* +0x0c */
} yglc_ct_req_t;

typedef struct {
    uint32_t flags;        /* +0x00 */
    uint32_t rgb;          /* +0x04 1..0xffffff                               */
    uint8_t  effect;       /* +0x08 */
    uint8_t  _pad09[3];
    uint32_t duration_ms;  /* +0x0c */
} yglc_rgb_req_t;

typedef struct {
    uint32_t flags;        /* +0x00 */
    int32_t  hue;          /* +0x04 -1..359 */
    int32_t  sat;          /* +0x08 -1..100 */
    uint8_t  effect;       /* +0x0c */
    uint8_t  _pad0d[3];
    uint32_t duration_ms;  /* +0x10 */
} yglc_hsv_req_t;

/* start_cf / stop_cf / scene "cf" payload (0x114) */
typedef struct {
    int32_t  count;        /* +0x000 */
    int32_t  action;       /* +0x004 */
    uint32_t flags;        /* +0x008 bit0 must be set                        */
    uint8_t  enable;       /* +0x00c set to 1 by start_cf                    */
    char     expr[0x103];  /* +0x00d "count,action,tuples..." (snprintf 0x100) */
    uint32_t start_opt;    /* +0x110 = 1, passed to FUN_100c17b8              */
} yglc_cf_req_t;

/* set_scene payload (0x224). Field meaning depends on type. */
typedef struct {
    uint32_t type;         /* +0x000 yglc_scene_type_t */
    union {
        struct {               /* COLOR */
            uint32_t flags;    /* +0x04 */
            uint32_t rgb;      /* +0x08 */
            int32_t  bright;   /* +0x0c */
            uint8_t  effect;   /* +0x10 */
            uint8_t  _p[3];
            uint32_t duration; /* +0x14 */
        } color;
        struct {               /* CT (note: odd field order) */
            uint32_t flags;    /* +0x04 */
            int32_t  ct;       /* +0x08 */
            uint8_t  effect;   /* +0x0c */
            uint8_t  _p[3];
            uint32_t duration; /* +0x10 */
            int32_t  bright;   /* +0x14 */
        } ct;
        struct {               /* HSV */
            uint32_t flags;    /* +0x04 */
            int32_t  hue;      /* +0x08 */
            int32_t  sat;      /* +0x0c */
            int32_t  bright;   /* +0x10 */
            uint8_t  effect;   /* +0x14 */
            uint8_t  _p[3];
            uint32_t duration; /* +0x18 */
        } hsv;
        yglc_cf_req_t cf;      /* CF: +0x04..+0x117 */
        struct {               /* DELAY_OFF */
            int32_t  bright;   /* +0x04 -1..100 */
            int32_t  minutes;  /* +0x08 != 0    */
        } delay_off;
        struct {               /* SET_PS ("nightlight") */
            char key[0x20];    /* +0x04 */
            char value[0x200]; /* +0x24 */
        } ps;
        struct {               /* BRIGHT / OFF / ON (ON/OFF use only flags_on_off) */
            uint32_t flags;    /* +0x04 */
            int32_t  bright;   /* +0x08 0..100 (0 => power off) */
            uint32_t effect_or_onoff_flags; /* +0x0c effect byte; ON/OFF: flags=1 */
            uint32_t duration; /* +0x10 */
        } bright;
        uint8_t raw[0x220];
    } u;
} yglc_scene_req_t;

typedef struct {
    uint8_t  type;         /* +0x00 yglc_cron_type_t (0..4) */
    uint8_t  _pad01[3];
    uint32_t extra;        /* +0x04 3rd param for types 2..4 */
    int32_t  minutes;      /* +0x08 > 0 */
    uint8_t  _rsv0c[8];
} yglc_cron_req_t;

typedef struct {
    int32_t count;         /* +0x00 1..4 */
    uint8_t types[4];      /* +0x04 */
} yglc_cron_sel_req_t;

typedef struct {
    uint32_t flags;        /* +0x00 */
    uint8_t  action;       /* +0x04 yglc_adjust_action_t */
    uint8_t  prop;         /* +0x05 yglc_adjust_prop_t   */
    uint8_t  _pad06[2];
    uint32_t duration_ms;  /* +0x08 (adjust_*: <= 60000) */
    int32_t  value;        /* +0x0c percentage for PCT_UP/PCT_DOWN */
    uint8_t  opt;          /* +0x10 adjust_*: 3rd param; set_adjust: 3rd param (<3) */
    uint8_t  _pad11[3];
} yglc_adjust_req_t;

typedef struct {
    char key[0x20];        /* +0x000 */
    char value[0x200];     /* +0x020 */
} yglc_ps_req_t;

typedef struct {
    uint8_t  enable;       /* +0x00 */
    char     host[0x11];   /* +0x01 */
    uint16_t port;         /* +0x12 */
} yglc_music_req_t;

typedef struct {
    uint32_t flags;        /* +0x00 */
    char     name[0x20];   /* +0x04 "power" / "wrgb" */
    uint32_t arg0;         /* +0x24 */
    uint32_t arg1;         /* +0x28 */
} yglc_test_req_t;

/* Queue slot (10 slots @ 0x20006918, 0x238 each; pointer is sent through the queue). */
#define YGLC_REQ_PAYLOAD_MAX 0x224u
typedef void (*yglc_req_done_cb_t)(uint32_t status, void *out, void *arg);
typedef struct {
    uint32_t           in_use;                         /* +0x000 */
    uint8_t            id;                             /* +0x004 yglc_req_id_t */
    uint8_t            _pad05[3];
    uint8_t            payload[YGLC_REQ_PAYLOAD_MAX];  /* +0x008 */
    yglc_req_done_cb_t done_cb;                        /* +0x22c */
    void              *done_arg;                       /* +0x230 */
    uint8_t            from_isr;                       /* +0x234 always 0 */
    uint8_t            _pad235[3];
} yglc_req_slot_t;
#define YGLC_REQ_SLOT_COUNT 10u

/* Core context @ 0x20007fc4 */
typedef struct {
    uint32_t _unk00;                                 /* +0x00 */
    void    *queue;                                  /* +0x04 xQueue(10, 4)  */
    uint8_t  deny[2][YGLC_REQ_COUNT];                /* +0x08 [settings->power][req id] */
} yglc_core_ctx_t;

/* ===================================================================== */
/*  Partial views of firmware objects (only fields used by this layer)    */
/* ===================================================================== */
typedef struct yglc_unit_view yglc_unit_view_t;

typedef struct {
    int32_t  ct;                 /* +0x000 */
    int32_t  max_ct;             /* +0x004 6500 */
    int32_t  min_ct;             /* +0x008 DL 2700, NL 1700 */
    uint8_t  _unk0c[8];
    uint8_t  rgb[4];             /* +0x014 */
    uint8_t  _unk18[8];
    int32_t  color_mode;         /* +0x020 */
    int32_t  bright;             /* +0x024 DL default 100, NL default 5 */
    uint8_t  _unk28[4];
    int32_t  flowing;            /* +0x02c */
    uint8_t  _unk30[0x100];      /* +0x030 flow string */
    void    *effect_mgr;         /* +0x130 */
    int32_t  off;                /* +0x134 channel power-off flag */
    const int32_t *bright_steps; /* +0x138 remote +/- table */
    int32_t  bright_steps_n;     /* +0x13c */
    const int32_t *ct_steps;     /* +0x140 remote "sun" table (DL only) */
    int32_t  ct_steps_n;         /* +0x144 */
    uint8_t  _unk148[8];
    const int32_t *knob_bright;  /* +0x150 seesaw table */
    int32_t  knob_bright_n;      /* +0x154 */
    const int32_t *knob_ct;      /* +0x158 seesaw table (DL only) */
    int32_t  knob_ct_n;          /* +0x15c */
    uint8_t  _unk160[0x18];
    uint32_t color_mode_mask;    /* +0x178 */
    void    *cb_effect_start;    /* +0x17c */
    void    *cb_effect_done;     /* +0x180 */
    void    *cb_state_sync;      /* +0x184 */
    yglc_unit_view_t *unit;      /* +0x188 */
    int32_t  ct_dir;             /* +0x18c remote "sun" ping-pong direction */
} yglc_channel_view_t;

struct yglc_unit_view {
    int32_t  off;                /* +0x000 1 = unit off, 0 = on */
    int32_t  nchan;              /* +0x004 2 */
    yglc_channel_view_t *ch[2];  /* +0x008 [0] = "dl", [1] = "nl" */
    int32_t  active;             /* +0x010 0 = DL, 1 = NL */
    uint8_t  _unk14[0x10];
    int32_t  flowing;            /* +0x024 */
    char     flow_params[0x100]; /* +0x028 */
    uint8_t  _unk128[4];
    int32_t  nl_status_cfg;      /* +0x12c persisted "nl_status" (power-on in NL) */
    int32_t  nl_active;          /* +0x130 runtime: NL currently on              */
    int32_t  nl_br_effective;    /* +0x134 reported as "nl_br"                    */
    uint8_t  nl_type;            /* +0x138 0 = separate NL channel (ceiling3)     */
    uint8_t  _pad139[3];
    int32_t  active_mode;        /* +0x13c reported "active_mode" (1 = NL)        */
    int32_t  active_bright;      /* +0x140 reported "active_bright"               */
};

typedef struct {
    char     vendor[0x10];       /* +0x000 "Yeelink"  */
    char     model[0x10];        /* +0x010 "ceiling3" */
    char     fw_ver[0x10];       /* +0x020 "48"       */
    char     hw_ver[0x10];       /* +0x030 "1.0"      */
    uint8_t  _unk40[0x10];       /* +0x040 mixer arg  */
    int32_t  rt_temp;            /* +0x050 */
    uint32_t power;              /* +0x054 0 = ON, 1 = OFF ("power" prop) */
    yglc_unit_view_t *unit;      /* +0x058 */
    uint8_t  _unk5c[4];
    uint32_t save_state;         /* +0x060 1 = restore last state on cold boot */
    int32_t  delayoff_left_min;  /* +0x064 "delayoff" */
    int32_t  delayoff_dflt_min;  /* +0x068 "delayoff_info" (default 10) */
    uint32_t night_delayoff_armed; /* +0x06c */
    uint32_t lan_ctrl;           /* +0x070 */
    char     name[0x40];         /* +0x074 */
    uint32_t miband_sleep;       /* +0x0b4 */
    uint32_t alert_mask;         /* +0x0b8 cfg_alert_ctrl bits */
    uint32_t music_on;           /* +0x0bc */
    uint8_t  _unkc0[4];
    int32_t  night_start_s;      /* +0x0c4 default 22:00:00 */
    int32_t  night_end_s;        /* +0x0c8 default 06:00:00 */
    uint8_t  night_enabled;      /* +0x0cc */
    char     nighttime[0x24];    /* +0x0cd "disable|220000|060000|00" */
    uint8_t  _padf1[3];
    int32_t  night_delayoff_min; /* +0x0f4 */
    uint32_t is_night_time;      /* +0x0f8 cached result of yglc_is_night_time */
    uint8_t  _unkfc[4];
    uint32_t init_power_opt;     /* +0x100 0 restore, 1 force ON, 2 keep OFF */
    uint32_t fw_fab;             /* +0x104 */
    uint32_t sub_model;          /* +0x108 */
    uint8_t  _unk10c[4];
    uint32_t trans_dflt_ms;      /* +0x110 "trans_default" interval */
    uint8_t  power_on_effect;    /* +0x114 */
    uint8_t  _pad115[3];
} yglc_settings_view_t;

/* miIO JSON-RPC parser context (pvPortCalloc(1, 0x68) in yglc_json_parser_init 0x100bc64c) */
typedef int (*yglc_rpc_reply_fn_t)(void *rpc_hdr, void *arg);
typedef struct {
    char     method[0x20];       /* +0x00 */
    uint8_t  jsmn[0x1c];         /* +0x20 jsmi parser (tokens) */
    void    *params_tok;         /* +0x3c */
    const char *json;            /* +0x40 */
    uint32_t json_len;           /* +0x44 */
    uint32_t param_idx;          /* +0x48 next positional param */
    void    *reply_arg;          /* +0x4c */
    yglc_rpc_reply_fn_t reply;   /* +0x50 */
    uint8_t  rpc_hdr[0x0c];      /* +0x54 copy of miio delegate arg (id at +0x54) */
    uint32_t out_len;            /* +0x60 */
    char    *out_buf;            /* +0x64 */
} yglc_rpc_parser_t;

typedef int (*yglc_rpc_handler_t)(yglc_rpc_parser_t *p);
typedef struct {
    uint32_t auto_reply;         /* +0x00 always 0 in fw */
    const char *name;            /* +0x04 */
    yglc_rpc_handler_t handler;  /* +0x08 */
} yglc_rpc_method_t;             /* table @ 0x20000aec, 32 entries */

typedef void (*yglc_ble_action_fn_t)(uint32_t arg);
typedef struct {
    uint32_t code;               /* +0x00 YGLC_KEY_CODE(type, key) */
    yglc_ble_action_fn_t fn;     /* +0x04 */
    uint32_t arg;                /* +0x08 */
} yglc_ble_key_t;                /* table @ 0x200008a4, 15 entries (count @ 0x20000a30) */

/* ===================================================================== */
/*  Exported clean API                                                    */
/* ===================================================================== */
/* RPC helpers / dispatcher */
int  yglc_rpc_pop_int(yglc_rpc_parser_t *p, int32_t *out);                 /* 0x100bc864 */
int  yglc_rpc_pop_str(yglc_rpc_parser_t *p, char *out, uint32_t cap);      /* 0x100bcc00 */
int  yglc_rpc_parse_effect_duration(yglc_rpc_parser_t *p, uint8_t *effect, uint8_t dflt_effect,
                                    uint32_t *duration, uint32_t dflt_duration); /* 0x100bd074 */
int  yglc_rpc_reply_status(int status, void *rpc_hdr, yglc_rpc_reply_fn_t reply, void *arg); /* 0x100bc33c */
void yglc_rpc_parser_free(yglc_rpc_parser_t *p);                            /* 0x100bc384 */
void yglc_rpc_on_request_done(uint32_t status, void *out, void *parser);   /* 0x100bc44c */
int  json_rpc_to_yglc(uint8_t req_id, const void *payload, uint32_t len,
                      yglc_req_done_cb_t cb, yglc_rpc_parser_t *p);         /* 0x100bbd44 */
bool yglc_rpc_dispatch(void *rpc_arg, yglc_rpc_reply_fn_t reply, void *reply_arg); /* 0x100bc7d0 */
bool yglc_lan_method_exposed(const char *method);                          /* 0x100c3814 part */

/* RPC method handlers */
int yglc_m_set_power(yglc_rpc_parser_t *p);      /* 0x100bd0f0 */
int yglc_m_toggle(yglc_rpc_parser_t *p);         /* 0x100bc3c8 */
int yglc_m_set_bright(yglc_rpc_parser_t *p);     /* 0x100bd2cc */
int yglc_m_set_ct_abx(yglc_rpc_parser_t *p);     /* 0x100bd4ac */
int yglc_m_set_scene(yglc_rpc_parser_t *p);      /* 0x100bd668 */
int yglc_m_set_default(yglc_rpc_parser_t *p);    /* 0x100bc398 */
int yglc_m_cron_add(yglc_rpc_parser_t *p);       /* 0x100bc8ac */
int yglc_m_start_cf(yglc_rpc_parser_t *p);       /* 0x100bd610 */
int yglc_m_stop_cf(yglc_rpc_parser_t *p);        /* 0x100bbd98 */
int yglc_m_set_adjust(yglc_rpc_parser_t *p);     /* 0x100bcd14 */
int yglc_m_adjust_bright(yglc_rpc_parser_t *p);  /* 0x100bcb00 */
int yglc_m_adjust_ct(yglc_rpc_parser_t *p);      /* 0x100bcb58 */
int yglc_m_set_ps(yglc_rpc_parser_t *p);         /* 0x100bce38 */

/* Queue / task */
int      yglc_post_request_async(uint8_t id, const void *payload, uint32_t len,
                                 yglc_req_done_cb_t cb, void *arg);         /* 0x100bdbc0 */
uint32_t yglc_proc_request(const uint8_t *req, void *out);                 /* 0x100bfad4 */
void     yglc_task(void *arg);                                              /* 0x100bff30 */

/* Leaf request handlers */
uint32_t yglc_req_power_on(const yglc_power_req_t *r);    /* 0x100bdcfa */
uint32_t yglc_req_power_off(const yglc_power_req_t *r);   /* 0x100bdd0e */
uint32_t yglc_req_toggle(void);                           /* 0x100bdd24 */
uint32_t yglc_req_set_bright(const yglc_bright_req_t *r); /* 0x100bdd7c */
uint32_t yglc_req_adjust(const yglc_adjust_req_t *r);     /* 0x100bdebc */
uint32_t yglc_req_set_default(void);                      /* 0x100be058 */
uint32_t yglc_scene_apply(const yglc_scene_req_t *s);     /* 0x100bfa60 */
void     yglc_reset_bright_to_max(void);                  /* 0x100bdcb0 */

/* set_ps / nighttime */
uint32_t yglc_set_ps(const yglc_ps_req_t *ps);            /* 0x100bf77c (Ghidra: yglc_set_trans_default) */
uint32_t yglc_ps_nightlight(const char *value);           /* 0x100be44c */
uint32_t yglc_ps_nighttime(const char *value);            /* 0x100be150 */
bool     yglc_is_night_time(void);                        /* 0x100be560 */
uint32_t yglc_post_nightlight_ps(int op, uint32_t bright);/* 0x100bf1e0 */

/* BLE */
uint32_t yglc_handle_ble_event(const uint8_t *mac, uint32_t eid, uint32_t len,
                               const uint8_t *edata);      /* 0x100bf5a8 */
void     yglc_seesaw_event(int type, int arg1, int arg0);  /* 0x100bf29c */
uint32_t yglc_ps_pseudo_beacon(const char *value);         /* 0x100bf740 */

/* Boot / power cycle */
uint32_t yglc_product_power_on_at_boot(void);              /* 0x100bac8c */
void     yglc_boot_decide_power(void);                     /* part of yglc_init_async 0x100bedb4 */
int32_t  yglc_fast_reboot_count_load(void);                /* 0x100bebb8 */
void     yglc_check_fast_reboot(int32_t threshold);        /* 0x100bebf0 */

/* NOT FIRMWARE CODE: proposal for "hybrid" brightness (see LIGHT_CMD.md) */
uint32_t yglc_hybrid_set_bright(int32_t bright, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

/* ---- Layout checks (verified offsets) ---- */
_Static_assert(sizeof(yglc_power_req_t) == 0x14, "power req 0x14");
_Static_assert(offsetof(yglc_power_req_t, mode) == 0x0c, "power mode +0xc");
_Static_assert(sizeof(yglc_bright_req_t) == 0x10, "bright req 0x10");
_Static_assert(sizeof(yglc_ct_req_t) == 0x10, "ct req 0x10");
_Static_assert(sizeof(yglc_hsv_req_t) == 0x14, "hsv req 0x14");
_Static_assert(sizeof(yglc_cf_req_t) == 0x114, "cf req 0x114");
_Static_assert(offsetof(yglc_cf_req_t, expr) == 0x0d, "cf expr +0xd");
_Static_assert(offsetof(yglc_cf_req_t, start_opt) == 0x110, "cf start_opt +0x110");
_Static_assert(sizeof(yglc_scene_req_t) == 0x224, "scene req 0x224");
_Static_assert(offsetof(yglc_scene_req_t, u.ct.bright) == 0x14, "scene ct bright +0x14");
_Static_assert(offsetof(yglc_scene_req_t, u.hsv.duration) == 0x18, "scene hsv dur +0x18");
_Static_assert(offsetof(yglc_scene_req_t, u.ps.value) == 0x24, "scene ps value +0x24");
_Static_assert(offsetof(yglc_scene_req_t, u.bright.duration) == 0x10, "scene bright dur +0x10");
_Static_assert(offsetof(yglc_scene_req_t, u.cf.start_opt) == 0x114, "scene cf start +0x114");
_Static_assert(sizeof(yglc_cron_req_t) == 0x14, "cron req 0x14");
_Static_assert(sizeof(yglc_adjust_req_t) == 0x14, "adjust req 0x14");
_Static_assert(offsetof(yglc_adjust_req_t, opt) == 0x10, "adjust opt +0x10");
_Static_assert(sizeof(yglc_ps_req_t) == 0x220, "ps req 0x220");
_Static_assert(offsetof(yglc_music_req_t, port) == 0x12, "music port +0x12");
_Static_assert(sizeof(yglc_test_req_t) == 0x2c, "test req 0x2c");
_Static_assert(sizeof(yglc_req_slot_t) == 0x238, "req slot 0x238");
_Static_assert(offsetof(yglc_req_slot_t, done_cb) == 0x22c, "slot cb +0x22c");
_Static_assert(offsetof(yglc_req_slot_t, from_isr) == 0x234, "slot flag +0x234");
_Static_assert(offsetof(yglc_core_ctx_t, deny) == 0x08, "core deny +0x8");
#if defined(__arm__)
_Static_assert(offsetof(yglc_channel_view_t, bright) == 0x24, "ch bright +0x24");
_Static_assert(offsetof(yglc_channel_view_t, flowing) == 0x2c, "ch flowing +0x2c");
_Static_assert(offsetof(yglc_channel_view_t, bright_steps) == 0x138, "ch steps +0x138");
_Static_assert(offsetof(yglc_channel_view_t, ct_steps) == 0x140, "ch ct steps +0x140");
_Static_assert(offsetof(yglc_channel_view_t, knob_ct) == 0x158, "ch knob ct +0x158");
_Static_assert(offsetof(yglc_channel_view_t, color_mode_mask) == 0x178, "ch mask +0x178");
_Static_assert(offsetof(yglc_channel_view_t, ct_dir) == 0x18c, "ch ct_dir +0x18c");
_Static_assert(offsetof(yglc_unit_view_t, active) == 0x10, "unit active +0x10");
_Static_assert(offsetof(yglc_unit_view_t, flowing) == 0x24, "unit flowing +0x24");
_Static_assert(offsetof(yglc_unit_view_t, nl_status_cfg) == 0x12c, "unit nl_status +0x12c");
_Static_assert(offsetof(yglc_unit_view_t, nl_active) == 0x130, "unit nl_active +0x130");
_Static_assert(offsetof(yglc_unit_view_t, nl_type) == 0x138, "unit nl_type +0x138");
_Static_assert(offsetof(yglc_unit_view_t, active_mode) == 0x13c, "unit active_mode +0x13c");
_Static_assert(offsetof(yglc_unit_view_t, active_bright) == 0x140, "unit active_bright +0x140");
_Static_assert(sizeof(yglc_settings_view_t) == 0x118, "settings 0x118");
_Static_assert(offsetof(yglc_settings_view_t, power) == 0x54, "settings power +0x54");
_Static_assert(offsetof(yglc_settings_view_t, unit) == 0x58, "settings unit +0x58");
_Static_assert(offsetof(yglc_settings_view_t, name) == 0x74, "settings name +0x74");
_Static_assert(offsetof(yglc_settings_view_t, nighttime) == 0xcd, "settings nighttime +0xcd");
_Static_assert(offsetof(yglc_settings_view_t, night_delayoff_min) == 0xf4, "settings ndo +0xf4");
_Static_assert(offsetof(yglc_settings_view_t, init_power_opt) == 0x100, "settings ipo +0x100");
_Static_assert(offsetof(yglc_settings_view_t, trans_dflt_ms) == 0x110, "settings trans +0x110");
_Static_assert(offsetof(yglc_rpc_parser_t, param_idx) == 0x48, "parser idx +0x48");
_Static_assert(offsetof(yglc_rpc_parser_t, reply) == 0x50, "parser reply +0x50");
_Static_assert(offsetof(yglc_rpc_parser_t, rpc_hdr) == 0x54, "parser hdr +0x54");
_Static_assert(sizeof(yglc_rpc_parser_t) == 0x68, "parser 0x68");
_Static_assert(sizeof(yglc_rpc_method_t) == 0x0c, "method entry 0xc");
_Static_assert(sizeof(yglc_ble_key_t) == 0x0c, "ble key entry 0xc");
#endif
