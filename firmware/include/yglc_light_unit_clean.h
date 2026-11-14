#pragma once
/*
 * Clean C11 reconstruction: Yeelight Ceiling3 (yeelink.light.ceiling3, 2.0.2_0048,
 * MT7697 Cortex-M4F) LIGHT UNIT / LIGHT CHANNEL / EFFECT MANAGER / TRANS MANAGER.
 *
 * Covers (segment 0, base 0x10071000) 0x100c06a0..0x100c26c8 of decomp/app/yglc_core.c
 * plus the nighttime predicate FUN_100be560 and the unit->settings notifier 0x100be6bc.
 * Reconstructed from the raw Thumb-2 image (capstone), NOT from the Ghidra C: Ghidra has
 * wrong boundaries in this area (12 functions are not in functions.csv at all, e.g.
 * 0x100c06d4, 0x100c0760, 0x100c1418, 0x100c1520..0x100c16bc, 0x100c1e62, 0x100c232c,
 * 0x100c2574, 0x100c25b4, 0x100c25d4, 0x100c2650; FUN_100c1de6/FUN_100c1ad0 etc. are
 * merged with their tail-callees).
 *
 * Object graph (built by yglc_setting_cache_alloc @0x100bad18):
 *
 *   settings(+0x58) -> unit "main" (0x164)
 *                        channel[0] "dl" (0x1b4, mode mask 0x04 = CT)  ---+
 *                        channel[1] "nl" (0x1b4, mode mask 0x80 = NL)  ---+--> effect manager (0x20)
 *                                                                              stack of effects (0x3c)
 *                                                                              trans manager (0xe0)
 *                                                                                mixer (opaque, 100 B)
 *                                                                                PWM group (opaque)
 *
 * The mixer (struct yglc_mixer) and PWM group (struct yglc_pwm_group) are owned by
 * yglc_light_mixer_clean.h; here they are opaque and accessed by vtable offset only, so
 * both headers can be included together (all local names are prefixed yglc_lu_).
 *
 * See decomp/LIGHT_UNIT.md for the function map and the answers (dl<->nl switching,
 * bright->mixer path, hybrid patch sites, power-on logic).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yglc_mixer;      /* yglc_mixer_t       (yglc_light_mixer_clean.h) */
struct yglc_pwm_group;  /* yglc_pwm_group_t   (yglc_light_mixer_clean.h) */
struct yglc_trans_view; /* yglc_trans_view_t  (yglc_light_mixer_clean.h) = view of yglc_trans_manager_t */

/* ------------------------------------------------------------------------------------ */
/* Enumerations                                                                          */
/* ------------------------------------------------------------------------------------ */

/* Effect types (effect->type, also used as bit masks by effect_pop_types). */
typedef enum {
    YGLC_EFFECT_STATIC   = 1,   /* one transition to a color (set_ct/set_bright/restore) */
    YGLC_EFFECT_FLOW     = 2,   /* color flow (list of tuples)                           */
    YGLC_EFFECT_OFF      = 4,   /* power off: transition to bright 0                     */
    YGLC_EFFECT_STATIC_8 = 8,   /* static without commit (channel_apply(..., as_type8=1)),
                                 * no caller in 2.0.2_0048 passes 1                      */
} yglc_effect_type_t;

/* Color modes (request mode / flow tuple mode). Channel mode_mask bit = 1 << mode. */
typedef enum {
    YGLC_CMODE_RGB      = 1,    /* unsupported by ceiling3 (no RGB channel)              */
    YGLC_CMODE_CT       = 2,
    YGLC_CMODE_HSV      = 3,    /* in flows: "keep last CT, change bright"               */
    YGLC_CMODE_FLOW     = 4,    /* in flows: bright only (from channel)                  */
    YGLC_CMODE_CT_BR    = 5,    /* in flows: CT + channel bright                         */
    YGLC_CMODE_SLEEP    = 7,    /* nightlight mode (nl channel), in flows: "sleep/hold"  */
    YGLC_CMODE_NL_FLOW  = 8,    /* in flows: nl tuple (needs mode-7 channel)             */
} yglc_color_mode_t;

#define YGLC_CH_MASK_DL  (1u << YGLC_CMODE_CT)      /* 0x04 */
#define YGLC_CH_MASK_NL  (1u << YGLC_CMODE_SLEEP)   /* 0x80 */

/* unit->nl_type (+0x138): how "moonlight" is realised. ceiling3 uses 0. */
typedef enum {
    YGLC_NL_TYPE_CHANNEL   = 0, /* separate nl channel (index 1) - ceiling3              */
    YGLC_NL_TYPE_RGB_SLEEP = 1, /* mode 7 on the RGB channel, color 0xff9800 bright 1     */
    YGLC_NL_TYPE_CT_SLEEP  = 2, /* mode 7 on the CT channel, ct = min_ct bright 1         */
    YGLC_NL_TYPE_NONE      = 3, /* no nightlight: power-on always daylight                */
} yglc_nl_type_t;

/* set_power "on" mode (miio set_power ... mode), yglc_light_unit_set_power_on_mode. */
typedef enum {
    YGLC_PWR_ON_NORMAL = 0,     /* restore (nighttime / nl_status may select nl)         */
    YGLC_PWR_ON_CT     = 1,
    YGLC_PWR_ON_RGB    = 2,     /* -> error 1                                            */
    YGLC_PWR_ON_HSV    = 3,     /* -> error 1                                            */
    YGLC_PWR_ON_FLOW   = 4,     /* resume last saved flow (channel->flow_str)            */
    YGLC_PWR_ON_NL     = 5,     /* moonlight + nl_status = 1                             */
} yglc_power_on_mode_t;

/* Flow end action (flow->action, 2nd number of the flow string). */
typedef enum {
    YGLC_FLOW_END_RECOVER = 0,
    YGLC_FLOW_END_STAY    = 1,
    YGLC_FLOW_END_OFF     = 2,  /* posts request 2 (power off) for unit mask           */
    YGLC_FLOW_END_OFF_1   = 3,  /* same, mask 1                                         */
} yglc_flow_end_t;

/* adjust_* actions (yglc_table_step @0x100c0578 for 0/1/4/5; delta % for 2/3). */
typedef enum {
    YGLC_ADJ_TABLE_UP   = 0,
    YGLC_ADJ_TABLE_DOWN = 1,
    YGLC_ADJ_INCREASE   = 2,
    YGLC_ADJ_DECREASE   = 3,
    YGLC_ADJ_CIRCLE     = 4,    /* 4 and 5: table up with wrap-around                     */
} yglc_adjust_action_t;

/* ------------------------------------------------------------------------------------ */
/* Generic list / timer                                                                   */
/* ------------------------------------------------------------------------------------ */

typedef struct yglc_lu_list {
    struct yglc_lu_list *next;  /* +0x00 */
    struct yglc_lu_list *prev;  /* +0x04 */
} yglc_lu_list_t;

/* yglc soft timer (0x100c354c add / 0x100c35a0 del / 0x100c358e is_running). */
typedef struct yglc_lu_timer {
    yglc_lu_list_t node;        /* +0x00 */
    uint32_t       elapsed;     /* +0x08 cleared on (re)arm                               */
    uint32_t       period;      /* +0x0c ticks of 10 ms; 0 = fire on next tick            */
    uint32_t       _unk_10[2];  /* +0x10 */
    uint32_t       repeat;      /* +0x18 0 = one-shot                                     */
    void         (*cb)(void *timer, struct yglc_trans_view *arg); /* +0x1c (only user: trans) */
    void          *arg;         /* +0x20 */
    uint32_t       _unk_24;     /* +0x24 */
} yglc_lu_timer_t;

/* ------------------------------------------------------------------------------------ */
/* Transition tuple (what the mixer receives)                                             */
/* ------------------------------------------------------------------------------------ */

typedef struct yglc_trans_tuple {
    uint16_t ct;                /* +0x00 CT (K) - mixer arg1 is the whole u32 at +0       */
    uint16_t _pad_02;           /* +0x02 */
    uint8_t  bright;            /* +0x04 1..100, 0 = off (target forced to 0)             */
    uint8_t  has_ct;            /* +0x05 1 = ct valid                                     */
    uint16_t _pad_06;           /* +0x06 */
    float    scale;             /* +0x08 forced to 1.0f by start_transition (mixer arg3)  */
    uint32_t duration_ms;       /* +0x0c clamped to [300, 3600000]                        */
    uint32_t step_ms;           /* +0x10 overwritten with trans->default_step_ms (10)     */
    uint32_t is_nl;             /* +0x14 1 -> mixer min_bright = trans->min_bright_nl      */
} yglc_trans_tuple_t;

/* Flow node, pvPortCalloc(1, 0x24); list sorted in string order. */
typedef struct yglc_flow_node {
    uint32_t           mode;    /* +0x00 yglc_color_mode_t                                */
    yglc_trans_tuple_t tuple;   /* +0x04 */
    yglc_lu_list_t     link;    /* +0x1c */
} yglc_flow_node_t;

/* ------------------------------------------------------------------------------------ */
/* Effect + effect manager                                                                */
/* ------------------------------------------------------------------------------------ */

struct yglc_light_effect;
struct yglc_light_channel;
struct yglc_light_unit;

/* Callback table copied into every effect (+0x28), 0x14 bytes. */
typedef struct yglc_effect_cbs {
    void (*on_start)(void *ctx, struct yglc_light_effect *e);        /* +0x00 after 1st trans */
    uint32_t (*before_trans)(void *ctx, struct yglc_light_effect *e);/* +0x04 */
    void (*on_finish)(void *ctx, struct yglc_light_effect *e);       /* +0x08 last trans done */
    int  (*flow_step_check)(void *ctx, struct yglc_light_effect *e); /* +0x0c !=0: step taken over */
    void  *ctx;                                                      /* +0x10 the channel     */
} yglc_effect_cbs_t;

typedef struct yglc_effect_flow {
    int32_t         count;      /* +0x04 number of transitions, 0 = infinite              */
    int32_t         iter;       /* +0x08 transitions done                                 */
    yglc_lu_list_t  nodes;      /* +0x0c yglc_flow_node_t::link list                      */
    yglc_lu_list_t *cur;        /* +0x14 current node link (NULL before first step)       */
    uint8_t         action;     /* +0x18 yglc_flow_end_t                                  */
    uint8_t         _pad_19[3];
    uint32_t        allow_switch; /* +0x1c 1: flow may migrate to the other channel      */
} yglc_effect_flow_t;

typedef struct yglc_light_effect {
    uint8_t type;               /* +0x00 yglc_effect_type_t                               */
    uint8_t _pad_01[3];
    union {
        yglc_trans_tuple_t tuple;   /* +0x04 (STATIC / OFF / STATIC_8)                    */
        yglc_effect_flow_t flow;    /* +0x04 (FLOW)                                       */
    } u;
    yglc_lu_list_t    stack;    /* +0x20 link in effect_mgr->stack                        */
    yglc_effect_cbs_t cbs;      /* +0x28 */
} yglc_light_effect_t;

struct yglc_trans_manager;

typedef struct yglc_effect_manager {
    yglc_light_effect_t       *top;   /* +0x00 currently running effect                   */
    yglc_lu_list_t             stack; /* +0x04 LIFO, head.next = top                      */
    struct yglc_trans_manager *trans; /* +0x0c */
    char                       name[16]; /* +0x10 "dl" / "nl"                             */
} yglc_effect_manager_t;

/* ------------------------------------------------------------------------------------ */
/* Trans manager (pvPortCalloc(1, 0xe0))                                                  */
/* ------------------------------------------------------------------------------------ */

typedef struct yglc_trans_cbs {
    void (*done)(void *ctx);    /* +0x00 (+0x44) last step reached: effect_on_trans_done  */
    void (*step)(void *ctx);    /* +0x04 (+0x48) every step (always NULL)                 */
    void  *ctx;                 /* +0x08 (+0x4c) effect manager                           */
} yglc_trans_cbs_t;

typedef struct yglc_trans_manager {
    char               name[16];          /* +0x00 "dl" / "nl"                            */
    yglc_lu_timer_t    timer;             /* +0x10 cb = continue_transition (0x100c02bc)  */
    int32_t            total_steps;       /* +0x38 */
    int32_t            cur_step;          /* +0x3c */
    uint32_t           step_ms;           /* +0x40 timer period = step_ms / 10            */
    yglc_trans_cbs_t   cbs;               /* +0x44 */
    uint8_t            _unk_50[8];        /* +0x50 */
    yglc_trans_tuple_t prev;              /* +0x58 tuple of the previous transition       */
    yglc_trans_tuple_t cur;               /* +0x70 tuple of the running transition        */
    uint32_t           from_pwm[4];       /* +0x88 */
    uint32_t           out_pwm[4];        /* +0x98 last value written to the pins         */
    uint32_t           target_pwm[4];     /* +0xa8 (+0xb4 = target_pwm[3])               */
    struct yglc_pwm_group *pwm;           /* +0xb8 */
    struct yglc_mixer  *mixer;            /* +0xbc */
    uint32_t           default_step_ms;   /* +0xc0 10 for dl and nl                       */
    uint32_t           committed_bright;  /* +0xc4 pending_bright copied here at the end  */
    uint32_t           pending_bright;    /* +0xc8 */
    uint8_t            curve_same_bright; /* +0xcc dl 2, nl 0 -> mixer set_trans_curve   */
    uint8_t            curve_new_bright;  /* +0xcd dl 2, nl 1                             */
    uint8_t            _pad_ce[2];
    uint32_t           min_bright_nl;     /* +0xd0 dl 15, nl 0 (tuple.is_nl != 0)          */
    uint32_t           min_bright;        /* +0xd4 dl 15, nl 0 (normal)                    */
    uint32_t           retarget;          /* +0xd8 1 after effect_retarget (smooth restart)*/
    uint32_t           _unk_dc;           /* +0xdc */
} yglc_trans_manager_t;

/* ------------------------------------------------------------------------------------ */
/* Light channel (pvPortCalloc(1, 0x1b4))                                                 */
/* ------------------------------------------------------------------------------------ */

/* Color request handed to yglc_light_channel_apply (16 bytes, also saved in unit+0x144). */
typedef struct yglc_color_req {
    uint32_t mode;              /* +0x00 yglc_color_mode_t                                */
    uint32_t value;             /* +0x04 ct / rgb / hue (only low 16 bits reach the mixer)*/
    uint32_t aux;               /* +0x08 sat (HSV); often uninitialised stack             */
    uint32_t bright;            /* +0x0c */
} yglc_color_req_t;

typedef struct yglc_light_channel {
    uint32_t ct;                /* +0x000 committed CT (dl 4000 at boot)                  */
    uint32_t max_ct;            /* +0x004 6500                                            */
    uint32_t min_ct;            /* +0x008 dl 2700, nl 1700                                */
    uint32_t _unk_00c[2];       /* +0x00c */
    uint32_t rgb;               /* +0x014 0x00RRGGBB                                      */
    uint32_t hue;               /* +0x018 */
    uint32_t sat;               /* +0x01c */
    uint32_t color_mode;        /* +0x020 2 (CT) on both channels                         */
    uint32_t bright;            /* +0x024 committed bright: dl "bright", nl "nl_br"       */
    uint32_t _unk_028;          /* +0x028 */
    uint32_t flow_active;       /* +0x02c */
    char     flow_str[256];     /* +0x030 last infinite flow ("count,action,d,m,v,b,...") */
    yglc_effect_manager_t *effects; /* +0x130 */
    uint32_t off;               /* +0x134 1 = channel dark                                */
    const int32_t *bright_table;     /* +0x138 adjust_bright steps (dl 4 entries, nl 6)    */
    uint32_t       bright_table_len; /* +0x13c */
    const int32_t *ct_table;         /* +0x140 adjust_ct steps                             */
    uint32_t       ct_table_len;     /* +0x144 */
    const uint32_t *rgb_table;       /* +0x148 adjust_rgb (NULL on ceiling3)               */
    uint32_t        rgb_table_len;   /* +0x14c */
    const void     *aux_table_a;     /* +0x150 set at init, reader not in this range       */
    uint32_t        aux_table_a_len; /* +0x154 15                                          */
    const void     *aux_table_b;     /* +0x158 */
    uint32_t        aux_table_b_len; /* +0x15c 16 (dl)                                     */
    const uint32_t *rgb_new_table;   /* +0x160 adjust_rgb_new (NULL)                       */
    uint32_t        rgb_new_table_len; /* +0x164 */
    char     name[16];          /* +0x168 */
    uint32_t mode_mask;         /* +0x178 bit (1 << color mode) supported                 */
    int  (*on_flow_step)(struct yglc_light_unit *u, struct yglc_light_channel *ch,
                         yglc_effect_flow_t *f);                       /* +0x17c */
    void (*on_flow_end)(struct yglc_light_unit *u, struct yglc_light_channel *ch,
                        yglc_effect_flow_t *f);                        /* +0x180 */
    void (*on_state_change)(struct yglc_light_unit *u, struct yglc_light_channel *ch); /* +0x184 */
    struct yglc_light_unit *owner;  /* +0x188 */
    uint32_t enabled;           /* +0x18c set to 1 by the constructor, never read here    */
    uint8_t  is_nl;             /* +0x190 last applied request had mode 7                 */
    uint8_t  _pad_191[3];
    uint32_t restore_saved;     /* +0x194 one-shot: restore unit->saved_req on nl exit    */
    uint32_t flow_last_mode;    /* +0x198 2 after a committed tuple                       */
    uint32_t flow_last_ct;      /* +0x19c */
    uint32_t _unk_1a0;          /* +0x1a0 */
    uint32_t last_bright;       /* +0x1a4 bright of the last started tuple                */
    uint32_t _unk_1a8[2];       /* +0x1a8 */
    uint32_t skip_resume_once;  /* +0x1b0 (dl) power_on restores instead of resuming;
                                 * writer not found in yglc_core.c                        */
} yglc_light_channel_t;

/* ------------------------------------------------------------------------------------ */
/* Light unit (pvPortCalloc(1, 0x164))                                                    */
/* ------------------------------------------------------------------------------------ */

#define YGLC_UNIT_MAX_CHANNELS 2

typedef struct yglc_light_unit {
    uint32_t off;               /* +0x000 "power": mirrors active channel->off            */
    int32_t  channel_count;     /* +0x004 2                                               */
    yglc_light_channel_t *channel[YGLC_UNIT_MAX_CHANNELS]; /* +0x008 [0]=dl, [1]=nl       */
    int32_t  active;            /* +0x010 index of the active channel                     */
    char     name[16];          /* +0x014 "main"                                          */
    uint32_t flowing;           /* +0x024 prop "flowing"                                  */
    char     flow_params[256];  /* +0x028 prop "flow_params"                              */
    uint32_t unit_index;        /* +0x128 bit in the API unit mask (main = 0)             */
    uint32_t nl_status;         /* +0x12c prop "nl_status" (saved): power-on goes to nl   */
    uint32_t nl_active;         /* +0x130 moonlight currently shown                       */
    uint32_t nl_br_effective;   /* +0x134 prop "nl_br_effective"                          */
    uint8_t  nl_type;           /* +0x138 yglc_nl_type_t (0)                              */
    uint8_t  _pad_139[3];
    uint32_t active_mode;       /* +0x13c prop "active_mode": 0 day, 1 moon               */
    uint32_t active_bright;     /* +0x140 prop "active_bright"                            */
    yglc_color_req_t saved_req; /* +0x144 last non-nl request (restored on nl exit)       */
    uint32_t aux_power;         /* +0x154 ORed into settings->power_off; writer unknown   */
    uint8_t  flow_switched;     /* +0x158 a flow migrated to another channel              */
    uint8_t  _pad_159[3];
    int32_t  flow_saved_active; /* +0x15c */
    uint32_t flow_saved_nl_status; /* +0x160 */
} yglc_light_unit_t;

/* ------------------------------------------------------------------------------------ */
/* View of the global settings (*(yglc_settings **)0x20006908, 0x118 bytes)               */
/* ------------------------------------------------------------------------------------ */

typedef struct yglc_lu_settings {
    uint8_t  _unk_000[0x54];
    uint32_t power_off;          /* +0x054 0 = on, 1 = off (prop "power")                 */
    yglc_light_unit_t *unit;     /* +0x058 */
    uint8_t  _unk_05c[4];
    uint32_t save_state;         /* +0x060 prop "save_state" (1)                          */
    uint8_t  _unk_064[4];
    uint32_t delayoff_dflt;      /* +0x068 */
    uint32_t night_autooff_armed;/* +0x06c */
    uint8_t  _unk_070[0x4c];
    uint32_t report_mute;        /* +0x0bc 1: yglc_report_props does nothing             */
    uint8_t  _unk_0c0[4];
    int32_t  night_start_s;      /* +0x0c4 seconds of day (22:00:00 = 79200)              */
    int32_t  night_end_s;        /* +0x0c8 (06:00:00 = 21600)                             */
    uint8_t  nighttime_enabled;  /* +0x0cc */
    char     nighttime_cfg[0x24];/* +0x0cd "disable|220000|060000|00"                     */
    uint8_t  _pad_0f1[3];
    uint32_t night_autooff_min;  /* +0x0f4 4th field of "nighttime"                       */
    uint32_t is_night_cached;    /* +0x0f8 prop "is_night_time"                           */
    uint8_t  _unk_0fc[4];
    uint32_t init_power_opt;     /* +0x100 1 = on after power cut, 2 = off                */
    uint32_t fw_fab;             /* +0x104 */
    uint32_t sub_model;          /* +0x108 */
    uint8_t  _unk_10c[4];
    uint32_t trans_default_ms;   /* +0x110 prop "trans_interval_dflt"                     */
    uint8_t  power_on_effect;    /* +0x114 */
    uint8_t  _unk_115[3];
} yglc_lu_settings_t;

/* ------------------------------------------------------------------------------------ */
/* Layout checks                                                                          */
/* ------------------------------------------------------------------------------------ */

#if UINTPTR_MAX == 0xffffffffu
_Static_assert(sizeof(yglc_lu_timer_t) == 0x28, "timer size");
_Static_assert(sizeof(yglc_trans_tuple_t) == 0x18, "tuple size");
_Static_assert(offsetof(yglc_trans_tuple_t, bright) == 0x04, "tuple.bright");
_Static_assert(offsetof(yglc_trans_tuple_t, duration_ms) == 0x0c, "tuple.duration");
_Static_assert(offsetof(yglc_trans_tuple_t, is_nl) == 0x14, "tuple.is_nl");
_Static_assert(sizeof(yglc_flow_node_t) == 0x24, "flow node size");
_Static_assert(offsetof(yglc_flow_node_t, link) == 0x1c, "flow node link");
_Static_assert(sizeof(yglc_effect_cbs_t) == 0x14, "effect cbs");
_Static_assert(offsetof(yglc_light_effect_t, u.flow.count) == 0x04, "effect.flow.count");
_Static_assert(offsetof(yglc_light_effect_t, u.flow.nodes) == 0x0c, "effect.flow.nodes");
_Static_assert(offsetof(yglc_light_effect_t, u.flow.cur) == 0x14, "effect.flow.cur");
_Static_assert(offsetof(yglc_light_effect_t, u.flow.action) == 0x18, "effect.flow.action");
_Static_assert(offsetof(yglc_light_effect_t, u.flow.allow_switch) == 0x1c, "effect.flow.allow");
_Static_assert(offsetof(yglc_light_effect_t, u.tuple.ct) == 0x04, "effect.tuple.ct");
_Static_assert(offsetof(yglc_light_effect_t, u.tuple.bright) == 0x08, "effect.tuple.bright");
_Static_assert(offsetof(yglc_light_effect_t, u.tuple.duration_ms) == 0x10, "effect.tuple.dur");
_Static_assert(offsetof(yglc_light_effect_t, u.tuple.is_nl) == 0x18, "effect.tuple.is_nl");
_Static_assert(offsetof(yglc_light_effect_t, stack) == 0x20, "effect.stack");
_Static_assert(offsetof(yglc_light_effect_t, cbs) == 0x28, "effect.cbs");
_Static_assert(sizeof(yglc_light_effect_t) == 0x3c, "effect size");
_Static_assert(sizeof(yglc_effect_manager_t) == 0x20, "effect manager size");
_Static_assert(offsetof(yglc_effect_manager_t, trans) == 0x0c, "effect_mgr.trans");
_Static_assert(offsetof(yglc_trans_manager_t, timer) == 0x10, "trans.timer");
_Static_assert(offsetof(yglc_trans_manager_t, total_steps) == 0x38, "trans.total_steps");
_Static_assert(offsetof(yglc_trans_manager_t, cbs) == 0x44, "trans.cbs");
_Static_assert(offsetof(yglc_trans_manager_t, prev) == 0x58, "trans.prev");
_Static_assert(offsetof(yglc_trans_manager_t, cur) == 0x70, "trans.cur");
_Static_assert(offsetof(yglc_trans_manager_t, from_pwm) == 0x88, "trans.from");
_Static_assert(offsetof(yglc_trans_manager_t, out_pwm) == 0x98, "trans.out");
_Static_assert(offsetof(yglc_trans_manager_t, target_pwm) == 0xa8, "trans.target");
_Static_assert(offsetof(yglc_trans_manager_t, pwm) == 0xb8, "trans.pwm");
_Static_assert(offsetof(yglc_trans_manager_t, mixer) == 0xbc, "trans.mixer");
_Static_assert(offsetof(yglc_trans_manager_t, default_step_ms) == 0xc0, "trans.step");
_Static_assert(offsetof(yglc_trans_manager_t, curve_same_bright) == 0xcc, "trans.curve");
_Static_assert(offsetof(yglc_trans_manager_t, min_bright_nl) == 0xd0, "trans.min_nl");
_Static_assert(offsetof(yglc_trans_manager_t, min_bright) == 0xd4, "trans.min");
_Static_assert(offsetof(yglc_trans_manager_t, retarget) == 0xd8, "trans.retarget");
_Static_assert(sizeof(yglc_trans_manager_t) == 0xe0, "trans size");
_Static_assert(sizeof(yglc_color_req_t) == 0x10, "req size");
_Static_assert(offsetof(yglc_light_channel_t, bright) == 0x24, "ch.bright");
_Static_assert(offsetof(yglc_light_channel_t, flow_active) == 0x2c, "ch.flow_active");
_Static_assert(offsetof(yglc_light_channel_t, flow_str) == 0x30, "ch.flow_str");
_Static_assert(offsetof(yglc_light_channel_t, effects) == 0x130, "ch.effects");
_Static_assert(offsetof(yglc_light_channel_t, off) == 0x134, "ch.off");
_Static_assert(offsetof(yglc_light_channel_t, bright_table) == 0x138, "ch.bright_table");
_Static_assert(offsetof(yglc_light_channel_t, ct_table) == 0x140, "ch.ct_table");
_Static_assert(offsetof(yglc_light_channel_t, rgb_new_table) == 0x160, "ch.rgb_new_table");
_Static_assert(offsetof(yglc_light_channel_t, name) == 0x168, "ch.name");
_Static_assert(offsetof(yglc_light_channel_t, mode_mask) == 0x178, "ch.mode_mask");
_Static_assert(offsetof(yglc_light_channel_t, on_flow_step) == 0x17c, "ch.on_flow_step");
_Static_assert(offsetof(yglc_light_channel_t, owner) == 0x188, "ch.owner");
_Static_assert(offsetof(yglc_light_channel_t, is_nl) == 0x190, "ch.is_nl");
_Static_assert(offsetof(yglc_light_channel_t, restore_saved) == 0x194, "ch.restore_saved");
_Static_assert(offsetof(yglc_light_channel_t, last_bright) == 0x1a4, "ch.last_bright");
_Static_assert(offsetof(yglc_light_channel_t, skip_resume_once) == 0x1b0, "ch.skip_resume");
_Static_assert(sizeof(yglc_light_channel_t) == 0x1b4, "channel size");
_Static_assert(offsetof(yglc_light_unit_t, active) == 0x10, "unit.active");
_Static_assert(offsetof(yglc_light_unit_t, flowing) == 0x24, "unit.flowing");
_Static_assert(offsetof(yglc_light_unit_t, unit_index) == 0x128, "unit.index");
_Static_assert(offsetof(yglc_light_unit_t, nl_status) == 0x12c, "unit.nl_status");
_Static_assert(offsetof(yglc_light_unit_t, nl_active) == 0x130, "unit.nl_active");
_Static_assert(offsetof(yglc_light_unit_t, nl_br_effective) == 0x134, "unit.nl_br_eff");
_Static_assert(offsetof(yglc_light_unit_t, nl_type) == 0x138, "unit.nl_type");
_Static_assert(offsetof(yglc_light_unit_t, active_mode) == 0x13c, "unit.active_mode");
_Static_assert(offsetof(yglc_light_unit_t, active_bright) == 0x140, "unit.active_bright");
_Static_assert(offsetof(yglc_light_unit_t, saved_req) == 0x144, "unit.saved_req");
_Static_assert(offsetof(yglc_light_unit_t, aux_power) == 0x154, "unit.aux_power");
_Static_assert(offsetof(yglc_light_unit_t, flow_switched) == 0x158, "unit.flow_switched");
_Static_assert(offsetof(yglc_light_unit_t, flow_saved_nl_status) == 0x160, "unit.saved_nl");
_Static_assert(sizeof(yglc_light_unit_t) == 0x164, "unit size");
_Static_assert(offsetof(yglc_lu_settings_t, power_off) == 0x54, "settings.power");
_Static_assert(offsetof(yglc_lu_settings_t, unit) == 0x58, "settings.unit");
_Static_assert(offsetof(yglc_lu_settings_t, night_autooff_armed) == 0x6c, "settings.+0x6c");
_Static_assert(offsetof(yglc_lu_settings_t, report_mute) == 0xbc, "settings.+0xbc");
_Static_assert(offsetof(yglc_lu_settings_t, night_start_s) == 0xc4, "settings.+0xc4");
_Static_assert(offsetof(yglc_lu_settings_t, nighttime_enabled) == 0xcc, "settings.+0xcc");
_Static_assert(offsetof(yglc_lu_settings_t, night_autooff_min) == 0xf4, "settings.+0xf4");
_Static_assert(offsetof(yglc_lu_settings_t, is_night_cached) == 0xf8, "settings.+0xf8");
_Static_assert(offsetof(yglc_lu_settings_t, init_power_opt) == 0x100, "settings.+0x100");
_Static_assert(offsetof(yglc_lu_settings_t, trans_default_ms) == 0x110, "settings.+0x110");
_Static_assert(sizeof(yglc_lu_settings_t) == 0x118, "settings size");
#endif

/* ------------------------------------------------------------------------------------ */
/* API (address of the original routine in the comment)                                  */
/* ------------------------------------------------------------------------------------ */

/* --- trans manager --- */
uint32_t yglc_trans_tuple_steps(yglc_trans_tuple_t *t);                              /* 0x100c06a0 */
void     yglc_trans_manager_retarget(yglc_trans_manager_t *tm, yglc_trans_tuple_t *t,
                                     int keep_from);                                 /* 0x100c06d4 */
int      yglc_trans_manager_start_transition(yglc_trans_manager_t *tm, yglc_trans_tuple_t *t,
                                             const yglc_trans_cbs_t *cbs);           /* 0x100c07d4 */
yglc_trans_manager_t *yglc_new_trans_manager(const char *name, struct yglc_mixer *mixer,
                                             struct yglc_pwm_group *pwm,
                                             uint8_t curve_new_bright, uint8_t curve_same_bright,
                                             uint32_t default_step_ms, uint32_t min_bright,
                                             uint32_t min_bright_nl);                /* 0x100c0dcc */

/* --- effects --- */
void     yglc_effect_manager_retarget(yglc_effect_manager_t *m, int keep_from);      /* 0x100c0760 */
void     yglc_light_effect_next_trans(yglc_effect_manager_t *m, int started);       /* 0x100c0910 */
void     yglc_effect_on_trans_done(void *mgr);                                       /* 0x100c0a04 */
int      yglc_effect_flow_chain_add(yglc_effect_flow_t *f, const yglc_flow_node_t *n);/* 0x100c0a14 */
void     yglc_effect_flow_chain_free(yglc_effect_flow_t *f);                         /* 0x100c0a80 */
void     yglc_effect_flow_add_duration(yglc_effect_flow_t *f, int32_t delta_ms);     /* 0x100c0abc */
int      yglc_effect_flow_set_bright(yglc_effect_flow_t *f, uint8_t bright);         /* 0x100c0af8 */
yglc_light_effect_t *yglc_light_effect_create_new(uint32_t type);                    /* 0x100c0b14 */
void     yglc_light_effect_free(yglc_light_effect_t **pe);                           /* 0x100c0b8c */
int      yglc_light_effect_parse_flow_string(const char *s, yglc_light_effect_t **out);/* 0x100c0bfc */
yglc_effect_manager_t *yglc_new_effect_manager(const char *name, yglc_trans_manager_t *tm); /* 0x100c0e50 */
void     yglc_light_effect_sync_update_to_trans(yglc_effect_manager_t *m, uint8_t bright); /* 0x100c0eb4 */
void     yglc_effect_flow_adjust_speed(yglc_effect_manager_t *m, int32_t delta_ms);  /* 0x100c0ecc */
void     yglc_effect_manager_stop(yglc_effect_manager_t *m);                         /* 0x100c0eec */
void     yglc_light_effect_push(yglc_effect_manager_t *m, yglc_light_effect_t *e, int start); /* 0x100c0ef4 */
void     yglc_light_effect_resume(yglc_effect_manager_t *m, int32_t duration_ms);    /* 0x100c0f24 */
void     yglc_light_effect_pop(yglc_effect_manager_t *m, int resume);                /* 0x100c0f4c */
void     yglc_light_effect_pop_types(yglc_effect_manager_t *m, uint8_t type_mask);   /* 0x100c0fcc */
void     yglc_light_effect_pop_finite_flows(yglc_effect_manager_t *m);               /* 0x100c0fee */
void     yglc_light_effect_start(yglc_effect_manager_t *m, yglc_light_effect_t *e,
                                 const yglc_effect_cbs_t *cbs);                      /* 0x100c1014 */

/* --- channel --- */
yglc_light_channel_t *yglc_new_light_channel(const char *name, yglc_effect_manager_t *m,
                                             uint32_t mode_mask);                    /* 0x100c10b4 */
int      yglc_channel_fill_flow_tuple(yglc_light_channel_t *ch, yglc_flow_node_t *n);/* 0x100c1120 */
uint32_t yglc_channel_before_trans(void *ch, yglc_light_effect_t *e);              /* 0x100c1204 */
int      yglc_light_channel_update_flow_string(yglc_light_channel_t *ch);            /* 0x100c1224 */
void     yglc_light_channel_commit_tuple(yglc_light_channel_t *ch,
                                         const yglc_trans_tuple_t *t, int commit);   /* 0x100c12f8 */
void     yglc_light_channel_on_effect_start(void *ch, yglc_light_effect_t *e);     /* 0x100c136c */
void     yglc_light_channel_flow_adjust_speed(yglc_light_channel_t *ch, int32_t delta_ms); /* 0x100c1418 */
int      yglc_light_channel_apply(yglc_light_channel_t *ch, yglc_color_req_t *req,
                                  uint32_t duration_ms, int as_type8);               /* 0x100c1434 */
int      yglc_light_channel_set_raw(yglc_light_channel_t *ch, const uint8_t rgbw[4], uint8_t arg); /* 0x100c1520 */
int      yglc_light_channel_set_rgb(yglc_light_channel_t *ch, const uint32_t *rgb,
                                    int32_t bright, uint32_t duration_ms);           /* 0x100c15d4 */
int      yglc_light_channel_set_nl_rgb(yglc_light_channel_t *ch, uint32_t duration_ms); /* 0x100c1640 */
int      yglc_light_channel_set_nl_ct(yglc_light_channel_t *ch, uint32_t duration_ms);  /* 0x100c1664 */
int      yglc_light_channel_set_hsv(yglc_light_channel_t *ch, const int32_t hs[2],
                                    int32_t bright, uint32_t duration_ms);           /* 0x100c1682 */
int      yglc_light_channel_set_ct(yglc_light_channel_t *ch, int32_t ct, int32_t bright,
                                   uint32_t duration_ms);                            /* 0x100c16bc */
int      yglc_light_channel_set_bright(yglc_light_channel_t *ch, int32_t bright,
                                       uint32_t duration_ms, int as_type8);          /* 0x100c16f4 */
int      yglc_light_channel_restore(yglc_light_channel_t *ch, uint32_t duration_ms); /* 0x100c1780 */
int      yglc_light_channel_start_flow(yglc_light_channel_t *ch, const char *flow,
                                       uint32_t allow_switch);                       /* 0x100c17b8 */
int      yglc_light_channel_exit_nl(yglc_light_channel_t *ch, uint32_t duration_ms,
                                    uint32_t restore_saved);                         /* 0x100c1854 */
int      yglc_light_channel_power_off(yglc_light_channel_t *ch, uint32_t duration_ms);/* 0x100c1868 */
int      yglc_light_channel_power_on(yglc_light_channel_t *ch, uint32_t duration_ms); /* 0x100c18d0 */

/* --- unit --- */
yglc_light_unit_t *yglc_new_light_unit(const char *name);                            /* 0x100c1920 */
int32_t  yglc_light_unit_find_channel(const yglc_light_unit_t *u, uint32_t mode);    /* 0x100c1978 */
int      yglc_light_unit_on_flow_step(yglc_light_unit_t *u, yglc_light_channel_t *ch,
                                      yglc_effect_flow_t *f);                        /* 0x100c19bc */
void     yglc_light_unit_on_channel_state(yglc_light_unit_t *u, yglc_light_channel_t *ch); /* 0x100c1ad0 */
int      yglc_light_unit_restore_all(yglc_light_unit_t *u, uint32_t duration_ms);    /* 0x100c1b94 */
void     yglc_light_unit_on_flow_end(yglc_light_unit_t *u, yglc_light_channel_t *ch,
                                     yglc_effect_flow_t *f);                         /* 0x100c1bc6 */
void     yglc_light_unit_attach_channel(yglc_light_unit_t *u, int32_t idx,
                                        yglc_light_channel_t *ch);                   /* 0x100c1cb0 */
int      yglc_light_unit_activate(yglc_light_unit_t *u, int32_t idx, uint32_t duration_ms); /* 0x100c1cdc */
int      yglc_light_unit_set_power(yglc_light_unit_t *u, uint32_t off, uint32_t duration_ms); /* 0x100c1d12 */
int      yglc_light_unit_set_bright(yglc_light_unit_t *u, int32_t bright,
                                    uint32_t duration_ms, int as_type8);             /* 0x100c1d4a */
int      yglc_light_unit_stop_flow(yglc_light_unit_t *u);                            /* 0x100c1d64 */
bool     yglc_light_unit_ct_in_range(yglc_light_unit_t *u, int32_t ct);              /* 0x100c1d90 */
void     yglc_light_unit_select_channel(yglc_light_unit_t *u, uint32_t mode,
                                        uint32_t duration_ms);                       /* 0x100c1db8 */
int      yglc_light_unit_set_raw(yglc_light_unit_t *u, const uint8_t rgbw[4], uint8_t arg); /* 0x100c1de6 */
int      yglc_light_unit_set_rgb(yglc_light_unit_t *u, const uint32_t *rgb, int32_t bright,
                                 uint32_t duration_ms);                              /* 0x100c1dfa */
int      yglc_light_unit_set_hsv(yglc_light_unit_t *u, const int32_t hs[2], int32_t bright,
                                 uint32_t duration_ms);                              /* 0x100c1e2e */
int      yglc_light_unit_set_ct_keep_bright(yglc_light_unit_t *u, int32_t ct,
                                            uint32_t duration_ms);                   /* 0x100c1e62 */
int      yglc_light_unit_set_ct(yglc_light_unit_t *u, int32_t ct, int32_t bright,
                                uint32_t duration_ms);                               /* 0x100c1e92 */
int      yglc_light_unit_adjust_ct(yglc_light_unit_t *u, uint32_t action, uint32_t duration_ms,
                                   uint32_t percent_of_range, int32_t delta);        /* 0x100c1ec8 */
int      yglc_light_unit_nl_set_bright(yglc_light_unit_t *u, uint32_t bright,
                                       uint32_t duration_ms, int switch_to_nl);      /* 0x100c2008 */
int      yglc_light_unit_enable_nl(yglc_light_unit_t *u, uint32_t bright,
                                   uint32_t duration_ms, uint32_t set_status);       /* 0x100c2050 */
void     yglc_light_unit_disable_nl(yglc_light_unit_t *u, uint32_t duration_ms,
                                    uint32_t clear_status, uint32_t restore_saved);  /* 0x100c20b4 */
int      yglc_light_unit_adjust_rgb_new(yglc_light_unit_t *u, uint32_t action,
                                        uint32_t duration_ms);                       /* 0x100c2164 */
int      yglc_light_unit_adjust_rgb(yglc_light_unit_t *u, uint32_t action,
                                    uint32_t duration_ms);                           /* 0x100c2248 */
int      yglc_light_unit_start_flow(yglc_light_unit_t *u, const char *flow,
                                    uint32_t allow_switch);                          /* 0x100c232c */
void     yglc_light_unit_flow_adjust_speed(yglc_light_unit_t *u, int32_t delta_ms);  /* 0x100c2366 */
int      yglc_light_unit_arm_night_autooff(void);                                    /* 0x100c237c */
int      yglc_light_unit_power_on(yglc_light_unit_t *u, uint32_t duration_ms);       /* 0x100c23b0 */
int      yglc_light_unit_set_power_on_mode(yglc_light_unit_t *u, uint32_t mode,
                                           uint32_t duration_ms);                    /* 0x100c243c */
int      yglc_light_unit_adjust_bright(yglc_light_unit_t *u, uint32_t action,
                                       uint32_t duration_ms, int32_t delta);         /* 0x100c24a8 */

/* --- API wrappers (unit mask, bit 0 = settings->unit; 0 means 1) --- */
int      yglc_light_api_power_off(uint32_t mask, uint32_t duration_ms);              /* 0x100c2484 */
int      yglc_light_api_power_on(uint32_t mask, uint32_t mode, uint32_t duration_ms);/* 0x100c2574 */
int      yglc_light_api_set_bright(uint32_t mask, int32_t bright, uint32_t duration_ms,
                                   int as_type8);                                    /* 0x100c2594 */
int      yglc_light_api_set_raw(uint32_t mask, const uint8_t rgbw[4], uint8_t arg); /* 0x100c25b4 */
int      yglc_light_api_set_rgb(uint32_t mask, uint32_t rgb, int32_t bright,
                                uint32_t duration_ms);                               /* 0x100c25d4 */
int      yglc_light_api_set_hsv(uint32_t mask, int32_t hue, int32_t sat, int32_t bright,
                                uint32_t duration_ms);                               /* 0x100c25fc */
bool     yglc_light_api_ct_in_range(uint32_t mask, int32_t ct);                      /* 0x100c2628 */
int      yglc_light_api_set_ct(uint32_t mask, int32_t ct, int32_t bright,
                               uint32_t duration_ms);                                /* 0x100c2650 */
int      yglc_light_api_adjust_ct(uint32_t mask, uint32_t action, uint32_t duration_ms,
                                  uint32_t percent_of_range, int32_t delta);         /* 0x100c2670 */
int      yglc_light_api_adjust_rgb(uint32_t mask, uint32_t action, uint32_t duration_ms,
                                   uint32_t unused, uint32_t variant);               /* 0x100c269c */
int      yglc_light_api_adjust_bright(uint32_t mask, uint32_t action, uint32_t duration_ms,
                                      int32_t delta);                                /* 0x100c26c8 */

/* --- outside the range, reconstructed because the logic depends on them --- */
int32_t  yglc_table_step(uint32_t action, int32_t value, const int32_t *table,
                         uint32_t len);                                              /* 0x100c0578 */
uint32_t yglc_rgb_distance(uint32_t a, uint32_t b);                                  /* 0x100c0238 */
const yglc_trans_tuple_t *yglc_effect_current_tuple(const yglc_light_effect_t *e);   /* 0x100c05e4 */
void     yglc_channel_on_effect_finish(void *ch, yglc_light_effect_t *e);          /* 0x100c05f4 */
int      yglc_channel_flow_step_check(void *ch, yglc_light_effect_t *e);           /* 0x100c021c */
uint32_t yglc_is_night_time(void);                                                   /* 0x100be560 */
void     yglc_unit_state_notify(void);                                               /* 0x100be6bc */

#ifdef __cplusplus
}
#endif
