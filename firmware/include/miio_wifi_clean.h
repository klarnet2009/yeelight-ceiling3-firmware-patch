#pragma once
/*
 * Clean C11 reconstruction of the Yeelight Ceiling3 (yeelink.light.ceiling3,
 * 2.0.2_0048, MT7697 Cortex-M4F) Wi-Fi & MiIO network management subsystem.
 *
 * Covers the complete miio_net layer (0x100ae818..0x100b1c72) plus
 * yglc_network_monitor (0x100bd918) and typed bindings for MTK Wi-Fi Service
 * (wifi_config_* / wifi_connection_*) and lwIP netif/DHCP calls.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Wi-Fi / MiIO operation modes (MTK wifi_api) ---- */
typedef enum {
    WIFI_OPMODE_STA = 1,
    WIFI_OPMODE_AP  = 2,
    WIFI_OPMODE_REP = 3,
    WIFI_OPMODE_MON = 4,
} wifi_opmode_t;

/* ---- High-level MiIO net states (g_miio_net_state @ 0x20004b24+) ---- */
typedef enum {
    MIIO_NET_STATE_UNPROV       = 0,
    MIIO_NET_STATE_UAP          = 1,
    MIIO_NET_STATE_DISCONNECTED = 2,
    MIIO_NET_STATE_LOCAL        = 3,
    MIIO_NET_STATE_CLOUD        = 4,
    MIIO_NET_STATE_UPDATING     = 5,
} miio_net_conn_state_t;

/* ---- Internal STA link progress (miio_net_ctx_t::wifi_state) ---- */
typedef enum {
    MIIO_WIFI_STA_IDLE       = 0,
    MIIO_WIFI_STA_SCANNED    = 2,
    MIIO_WIFI_STA_CONNECTING = 3,
    MIIO_WIFI_STA_CONNECTED  = 4,
    MIIO_WIFI_STA_GOT_IP     = 5,
} miio_wifi_sta_state_t;

/* ---- Monitor queue event IDs (miio_net_post_event @ 0x100af2e0) ---- */
typedef enum {
    MIIO_WIFI_EVENT_CONFIG_ROUTER    = 0,
    MIIO_WIFI_EVENT_SCAN_DONE        = 1,
    MIIO_WIFI_EVENT_STA_START        = 2,
    MIIO_WIFI_EVENT_STA_STOP         = 3,
    MIIO_WIFI_EVENT_STA_CONNECTED    = 4,
    MIIO_WIFI_EVENT_STA_DISCONNECTED = 5,
    MIIO_WIFI_EVENT_STA_GOT_IP       = 6,
    MIIO_WIFI_EVENT_STA_LOST_IP      = 7,
    MIIO_WIFI_EVENT_AP_START         = 8,
    MIIO_WIFI_EVENT_AP_STOP          = 9,
} miio_wifi_event_id_t;

/* ---- MTK driver callback events (wifi_cb_event_handler @ 0x100af370) ---- */
typedef enum {
    WIFI_EVT_IOT_CONNECTED          = 0,
    WIFI_EVT_IOT_SCAN_COMPLETE      = 1,
    WIFI_EVT_IOT_DISCONNECTED       = 2,
    WIFI_EVT_IOT_PORT_SECURE        = 3,
    WIFI_EVT_IOT_INIT_COMPLETE      = 6,
    WIFI_EVT_IOT_CONNECT_FAILED     = 8,
} mtk_wifi_event_t;

/* ---- Doubly linked list node (Linux-style list_head) ---- */
typedef struct miio_list_head {
    struct miio_list_head *next;
    struct miio_list_head *prev;
} miio_list_head_t;

/* ---- Cached AP / repeater entry (0x2c = 44 bytes, allocated per scan hit) ---- */
typedef struct {
    miio_list_head_t node;      /* +0x00..+0x07 */
    const char      *ssid;      /* +0x08 pointer into scan table slot or context */
    uint8_t          bssid[6];  /* +0x0c..+0x11 */
    uint8_t          channel;   /* +0x12 primary channel */
    uint8_t          _pad_13;   /* +0x13 */
    int8_t           rssi;      /* +0x14 signed dBm */
    uint8_t          _pad_15[3];/* +0x15..+0x17 */
    uint32_t         auth_mode; /* +0x18 */
    uint32_t         enc_type;  /* +0x1c */
    uint8_t          _pad_20[8];/* +0x20..+0x27 */
    uint8_t          active;    /* +0x28: 1 = candidate, 0 = failed/blacklisted */
    uint8_t          _pad_29[3];/* +0x29..+0x2b */
} miio_wifi_ap_entry_t;

/* ---- Active AP snapshot inside miio_net_ctx_t (+0xd0..+0xef, 0x20 bytes) ---- */
typedef struct {
    const char *ssid;           /* +0x00 (+0xd0) */
    uint8_t     bssid[6];       /* +0x04 (+0xd4) */
    uint8_t     channel;        /* +0x0a (+0xda) */
    uint8_t     _pad_0b;        /* +0x0b (+0xdb) */
    int8_t      rssi;           /* +0x0c (+0xdc) */
    uint8_t     _pad_0d[0x13];  /* +0x0d..+0x1f */
} miio_wifi_cur_ap_t;

/* ---- Monitor queue message (12 bytes) ---- */
typedef struct {
    uint8_t  event;
    uint8_t  _pad[3];
    uint32_t arg0;
    uint32_t arg1;
} miio_wifi_msg_t;

typedef void (*miio_net_state_cb_t)(uint32_t state, uint32_t err, void *user_ctx);

typedef struct {
    miio_net_state_cb_t fn;
    void               *ctx;
} miio_net_cb_slot_t;

/* ---- Exact 0x1d4-byte MiIO Network & Wi-Fi Context (@ 0x20004b24) ---- */
typedef struct {
    void               *miio_handle;                /* +0x000 */
    uint8_t             factory_mode;               /* +0x004 */
    uint8_t             mac[6];                     /* +0x005 */
    uint8_t             _pad_0b[0x21];              /* +0x00b..+0x02b */
    uint8_t             provisioned;                /* +0x02c */
    char                ssid[33];                   /* +0x02d */
    char                ap_ssid[33];                /* +0x04e */
    char                password[65];               /* +0x06f */
    char                country_code[3];            /* +0x0b0 */
    uint8_t             schan;                      /* +0x0b3 */
    uint8_t             nchan;                      /* +0x0b4 */
    uint8_t             max_tx_dbm;                 /* +0x0b5 */
    uint8_t             _pad_b6[2];                 /* +0x0b6 */
    uint32_t            scan_interval_ms;           /* +0x0b8 (200..1000) */
    uint8_t             sta_scan_pending;           /* +0x0bc */
    uint8_t             _pad_bd[3];                 /* +0x0bd */
    miio_list_head_t    ap_list;                    /* +0x0c0 */
    uint32_t            ap_count;                   /* +0x0c8 */
    uint32_t            connect_count;              /* +0x0cc */
    miio_wifi_cur_ap_t  current_ap;                 /* +0x0d0..+0x0ef */
    uint8_t             wifi_state;                 /* +0x0f0 (miio_wifi_sta_state_t) */
    uint8_t             _pad_f1[3];                 /* +0x0f1 */
    char                config_type[16];            /* +0x0f4 */
    char                bindkey[65];                /* +0x104 */
    uint8_t             _pad_145[3];                /* +0x145 */
    uint32_t            disable_local_restore;      /* +0x148 */
    uint8_t             monitor_enabled;            /* +0x14c */
    uint8_t             _pad_14d[3];                /* +0x14d */
    void               *monitor_thread;             /* +0x150 */
    void               *monitor_queue;              /* +0x154 */
    uint32_t            last_tick_ms;               /* +0x158 */
    uint32_t            work_time_s;                /* +0x15c */
    uint32_t            heap_log_s;                 /* +0x160 */
    uint32_t            last_free_heap;             /* +0x164 */
    uint32_t            ap_timeout_s;               /* +0x168 */
    uint32_t            sta_offline_s;              /* +0x16c */
    uint32_t            sta_offline_timeout_s;      /* +0x170 (default 900s) */
    uint32_t            offline_count;              /* +0x174 */
    uint32_t            offline_window_s;           /* +0x178 */
    uint32_t            offline_history[10];        /* +0x17c..+0x1a3 */
    uint32_t            offline_count_threshold;    /* +0x1a4 (default 10) */
    uint32_t            time_unprov_s;              /* +0x1a8 */
    uint32_t            time_offline_s;             /* +0x1ac */
    uint32_t            time_online_s;              /* +0x1b0 */
    uint32_t            ntp_tick_s;                 /* +0x1b4 */
    uint8_t             ever_online;                /* +0x1b8 */
    uint8_t             _pad_1b9[3];                /* +0x1b9 */
    miio_net_cb_slot_t *callbacks;                  /* +0x1bc */
    uint32_t            callback_count;             /* +0x1c0 */
    void               *mutex;                      /* +0x1c4 */
    uint8_t             net_started;                /* +0x1c8 */
    uint8_t             disconnect_by_self;         /* +0x1c9 */
    uint8_t             wifi_init_done;             /* +0x1ca */
    uint8_t             scan_timer_created;         /* +0x1cb */
    void               *scan_timer;                 /* +0x1cc */
    uint8_t             target_opmode;              /* +0x1d0 */
    uint8_t             _pad_1d1[3];                /* +0x1d1..+0x1d3 */
} miio_net_ctx_t;

#if defined(__arm__) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4)
_Static_assert(sizeof(miio_wifi_ap_entry_t) == 0x2c, "ap_entry size");
_Static_assert(offsetof(miio_wifi_ap_entry_t, active) == 0x28, "ap_entry active offset");
_Static_assert(sizeof(miio_wifi_cur_ap_t) == 0x20, "cur_ap size");
_Static_assert(offsetof(miio_net_ctx_t, provisioned) == 0x2c, "provisioned offset");
_Static_assert(offsetof(miio_net_ctx_t, ap_list) == 0xc0, "ap_list offset");
_Static_assert(offsetof(miio_net_ctx_t, current_ap) == 0xd0, "current_ap offset");
_Static_assert(offsetof(miio_net_ctx_t, wifi_state) == 0xf0, "wifi_state offset");
_Static_assert(offsetof(miio_net_ctx_t, monitor_queue) == 0x154, "monitor_queue offset");
_Static_assert(offsetof(miio_net_ctx_t, sta_offline_timeout_s) == 0x170, "timeout offset");
_Static_assert(offsetof(miio_net_ctx_t, offline_history) == 0x17c, "history offset");
_Static_assert(offsetof(miio_net_ctx_t, mutex) == 0x1c4, "mutex offset");
_Static_assert(offsetof(miio_net_ctx_t, target_opmode) == 0x1d0, "target_opmode offset");
_Static_assert(sizeof(miio_net_ctx_t) == 0x1d4, "miio_net_ctx_t exact size 0x1d4");
#endif

/* ---- Clean reconstructed Wi-Fi subsystem functions ---- */
int      clean_miio_wifi_escape_ssid(const char *ssid, char *out);
bool     clean_miio_sta_monitor_ntp_tick(miio_net_ctx_t *ctx, uint32_t elapsed_s);
void     clean_miio_sta_monitor_reset_offline(miio_net_ctx_t *ctx);
void     clean_miio_sta_monitor_reset_history(miio_net_ctx_t *ctx);
void     clean_miio_net_state_change(miio_net_ctx_t *ctx, uint32_t new_state, uint32_t err);
bool     clean_sta_monitor_offline_timeout(miio_net_ctx_t *ctx, uint32_t elapsed_s);
int      clean_miio_wifi_scan_start(miio_net_ctx_t *ctx);
void     clean_miio_wifi_disconnect_clean(miio_net_ctx_t *ctx);
int      clean_miio_wifi_connect(miio_net_ctx_t *ctx);
int      clean_miio_wifi_try_reconnect(miio_net_ctx_t *ctx);
void     clean_miio_net_post_event(miio_wifi_event_id_t evt);
uint32_t clean_miio_wifi_scan_timeout(void);
uint32_t clean_wifi_cb_event_handler(uint32_t event_id, const uint8_t *payload);
void     clean_ip_ready_callback(const void *netif_ptr);
uint8_t  clean_miio_net_get_wifi_state(void);
void     clean_lwip_net_stop(int opmode);
void     clean_lwip_net_start(int opmode);
int      clean_wifi_set_opmode(uint8_t opmode);
int      clean_miio_net_stop(void);
int      clean_miio_net_start(void);
void     clean_miio_wifi_scan_finish(miio_net_ctx_t *ctx);
bool     clean_sta_monitor_offline_countout(miio_net_ctx_t *ctx, uint32_t elapsed_s);
void     clean_yglc_network_monitor(void);

#ifdef __cplusplus
}
#endif
