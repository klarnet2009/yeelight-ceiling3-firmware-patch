/*
 * Clean, human-readable C11 reconstruction of the Wi-Fi & MiIO Network Manager
 * for Yeelight Ceiling3 (yeelink.light.ceiling3, 2.0.2_0048, MT7697 Cortex-M4F).
 *
 * Original Ghidra artifacts (miio_net.c + helper routines split into
 * miio_net_ota.c + yglc_core.c) consolidated into one clean, strictly typed,
 * warning-free C11 module with explicit bug & root-cause annotations.
 */

#include "../include/miio_wifi_clean.h"
#include "../include/ceiling3_sdk.h"

/* ---- Standard libc / FreeRTOS / ROM symbols resolved by ceiling3_symbols.ld ---- */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
int   strncmp(const char *a, const char *b, size_t n);
char *strncpy(char *dst, const char *src, size_t n);
char *strcpy(char *dst, const char *src);
int   snprintf(char *buf, size_t size, const char *fmt, ...);

void *pvPortMalloc(size_t size);
void *pvPortCalloc(size_t nmemb, size_t size);
void  vPortFree(void *ptr);
uint32_t xPortGetFreeHeapSize(void);
int   xQueueGenericSend(void *queue, const void *item, uint32_t wait_ms, int copy_pos);

int   arch_os_mutex_get(void *mtx, uint32_t wait_ms);
int   arch_os_mutex_put(void *mtx);
int   arch_os_timer_create(void **timer, const char *name, uint32_t period_ms,
                           void *cb, void *arg, int auto_reload, int start_now);
int   arch_os_timer_activate(void *timer);
int   arch_os_timer_deactivate(void *timer);
int   arch_os_async_call(void *fn, void *arg, uint32_t delay_ms);
int   arch_os_queue_recv(void *q, void *msg, uint32_t wait_ms);
int   arch_os_queue_send(void *q, const void *msg, uint32_t wait_ms);

int   miio_ntp(void *miio_handle, void *cb, void *arg, uint32_t timeout_ms);
void  miio_set_event(void *miio_handle, const char *evt);

/* ---- Named wrappers over stripped MTK Wi-Fi Service / lwIP / MiIO ROM symbols ---- */
int      FUN_100e67e0(uint8_t *opmode);                          /* wifi_config_get_opmode */
int      FUN_100e682c(uint32_t opmode);                          /* wifi_config_set_opmode */
int      FUN_100e6a94(uint32_t port, const char *ssid, uint8_t len); /* wifi_config_set_ssid */
int      FUN_100e6b58(uint8_t *bssid_out);                       /* wifi_config_get_bssid */
int      FUN_100e6bc0(const uint8_t *bssid);                     /* wifi_config_set_bssid */
int      FUN_100e69e4(uint32_t port, char *ssid, uint8_t *len);  /* wifi_config_get_ssid */
int      FUN_100e6df8(uint32_t port, uint8_t channel);           /* wifi_config_set_channel */
int      FUN_100e73e0(const void *cc_cfg);                       /* wifi_config_set_country_code */
int      FUN_100e7938(uint32_t port, const char *psk, uint8_t len); /* wifi_config_set_wpa_psk_key */
int      FUN_100e7ba4(uint32_t port, const void *pmk_cfg);       /* wifi_config_set_pmk */
int      FUN_100e7e34(void);                                     /* wifi_connection_disconnect_ap */
int      FUN_100e813c(uint8_t on_off);                           /* wifi_config_set_radio */
void     FUN_100e831c(void);                                     /* wifi_connection_stop_scan */
int      FUN_100e8348(const char *ssid, uint8_t ssid_len, const uint8_t *bssid,
                      uint8_t scan_mode, uint8_t scan_option);   /* wifi_connection_start_scan */
int      FUN_100e881c(void);                                     /* wifi_config_reload_setting */
int      FUN_100e90e0(void *table, uint8_t max_entries);         /* wifi_connection_scan_init */
int      FUN_100e9130(void);                                     /* wifi_connection_scan_deinit */

uint8_t *FUN_10083558(int idx);                                  /* netif_by_index(1=ap1, 2=st2) */
void     FUN_10076598(void *netif, const void *ip, const void *mask, const void *gw); /* netif_set_addr */
void     FUN_1007674c(void *netif);                              /* netif_set_up */
void     FUN_100767a0(void *netif, void (*cb)(const void *));    /* netif_set_status_callback */
void     FUN_100767ac(void *netif);                              /* netif_set_link_up */
void     FUN_1007683c(void *netif);                              /* netif_set_link_down */
int      FUN_1007ced8(void *netif);                              /* dhcp_start */
void     FUN_1007d1b8(void *netif);                              /* dhcp_stop */
void     FUN_1007ce18(void *netif);                              /* dhcp_release */
const char *FUN_1007f418(const void *ip4_addr);                  /* ip4addr_ntoa */
void     FUN_1009b178(void);                                     /* dhcpd_stop */
void     FUN_1009aea4(const void *ap_cfg);                       /* dhcpd_start */

int      FUN_100ba16c(void);                                     /* yglc_dhcp_enabled */
int      FUN_100ba1ac(void *cfg_out);                            /* tcpip_config_get */
void     FUN_100ba248(const void *in, void *out);                /* dhcpd_settings_build */
void     FUN_100bf038(void);                                     /* yglc_on_ip_ready */
int      FUN_100a850c(void);                                     /* arch_net_st2_ip_is_zero */
void     FUN_100a7ab8(char *out, size_t cap, const uint8_t *mac, size_t n, uint8_t sep); /* mac_to_str */
uint8_t  FUN_100f72dc(void *miio_handle);                        /* miio_get_mode */
void     FUN_100f78b0(void *miio_handle, uint32_t mode);         /* miio_set_mode */
int      FUN_100f78f0(void *miio_handle, const char *key, const char *val, uint32_t len); /* miio_report_err */
bool     FUN_100f8a78(void *miio_handle);                        /* miio_ota_is_busy */
uint8_t  FUN_100f9f04(void *miio_handle);                        /* miio_ot_is_online */
void     FUN_100f9740(void *miio_handle);                        /* miio_enable_prov_led */
void     FUN_100f9768(void *miio_handle);                        /* miio_disable_prov_led */

/* Alias table matching binary addresses cleanly */
#define wifi_config_get_opmode      FUN_100e67e0
#define wifi_config_set_opmode      FUN_100e682c
#define wifi_config_set_ssid        FUN_100e6a94
#define wifi_config_get_bssid       FUN_100e6b58
#define wifi_config_set_bssid       FUN_100e6bc0
#define wifi_config_get_ssid        FUN_100e69e4
#define wifi_config_set_channel     FUN_100e6df8
#define wifi_config_set_country     FUN_100e73e0
#define wifi_config_set_psk         FUN_100e7938
#define wifi_config_set_pmk         FUN_100e7ba4
#define wifi_conn_disconnect_ap     FUN_100e7e34
#define wifi_config_set_radio       FUN_100e813c
#define wifi_conn_stop_scan         FUN_100e831c
#define wifi_conn_start_scan        FUN_100e8348
#define wifi_config_reload_setting  FUN_100e881c
#define wifi_conn_scan_init         FUN_100e90e0
#define wifi_conn_scan_deinit       FUN_100e9130

#define lwip_netif_find_idx         FUN_10083558
#define lwip_netif_set_addr         FUN_10076598
#define lwip_netif_set_up           FUN_1007674c
#define lwip_netif_set_status_cb    FUN_100767a0
#define lwip_netif_set_link_up      FUN_100767ac
#define lwip_netif_set_link_down    FUN_1007683c
#define lwip_dhcp_start             FUN_1007ced8
#define lwip_dhcp_stop              FUN_1007d1b8
#define lwip_dhcp_release           FUN_1007ce18
#define lwip_ip4_ntoa               FUN_1007f418

/* ---- Firmware static globals (host-testable or mapped to exact SRAM) ---- */
static miio_net_ctx_t   g_clean_miio_net_ctx;
static uint8_t          g_clean_net_state;
static uint8_t          g_clean_net_err;
static void            *g_clean_sta_sem;
static const uint32_t   g_zero_ip4 = 0;

/* Scan result slot layout in MTK scan_table (0x3a = 58 bytes per AP) */
typedef struct {
    uint8_t valid;         /* +0x00 (puVar15[-2] when puVar15 points to ssid) */
    int8_t  rssi;          /* +0x01 (puVar15[-1]) */
    char    ssid[33];      /* +0x02..+0x22 (puVar15[0..0x20]) */
    uint8_t bssid[6];      /* +0x23..+0x28 (puVar15[0x21..0x26]) */
    uint8_t channel;       /* +0x29 (puVar15[0x27]) */
    uint8_t _reserved[2];  /* +0x2a..+0x2b (puVar15[0x28..0x29], bss_type/wps) */
    uint8_t auth_mode;     /* +0x2c (puVar15[0x2a]) */
    uint8_t enc_type;      /* +0x2d (puVar15[0x2b]) */
    uint8_t _pad[12];      /* +0x2e..+0x39 */
} mtk_scan_ap_t;

_Static_assert(sizeof(mtk_scan_ap_t) == 0x3au, "MTK scan AP slot must be 58 bytes");
_Static_assert(offsetof(mtk_scan_ap_t, auth_mode) == 0x2cu, "auth_mode offset mismatch");
_Static_assert(offsetof(mtk_scan_ap_t, enc_type)  == 0x2du, "enc_type offset mismatch");

static mtk_scan_ap_t g_clean_scan_table[20]; /* 20 * 0x3a = 0x488 bytes */

/* Forward helpers */
static void lock_net(miio_net_ctx_t *ctx)
{
    if (ctx && ctx->mutex) {
        arch_os_mutex_get(ctx->mutex, 0xffffffffu);
    }
}

static void unlock_net(miio_net_ctx_t *ctx)
{
    if (ctx && ctx->mutex) {
        arch_os_mutex_put(ctx->mutex);
    }
}

static void clear_ap_list_locked(miio_net_ctx_t *ctx)
{
    miio_list_head_t *cur = ctx->ap_list.next;
    while (cur != &ctx->ap_list) {
        miio_list_head_t *nxt = cur->next;
        cur->next->prev = cur->prev;
        cur->prev->next = cur->next;
        cur->next = NULL;
        cur->prev = NULL;
        vPortFree(cur);
        cur = nxt;
    }
    ctx->ap_count = 0;
}

static uint32_t count_active_aps_locked(const miio_net_ctx_t *ctx)
{
    uint32_t active = 0;
    for (const miio_list_head_t *it = ctx->ap_list.next; it != &ctx->ap_list; it = it->next) {
        const miio_wifi_ap_entry_t *ap = (const miio_wifi_ap_entry_t *)it;
        if (ap->active != 0u) {
            active++;
        }
    }
    return active;
}

/* --------------------------------------------------------------------------
 * clean_miio_wifi_escape_ssid (@ 0x100ae818)
 * Escapes '/', '\\', '"' in up to 32-byte SSID for JSON telemetry.
 * -------------------------------------------------------------------------- */
int clean_miio_wifi_escape_ssid(const char *ssid, char *out)
{
    char *dst = out;
    for (const char *src = ssid; *src != '\0' && (src - ssid) < 0x20; src++) {
        if (*src == '/' || *src == '\\' || *src == '\"') {
            *dst++ = '\\';
        }
        *dst++ = *src;
    }
    *dst = '\0';
    return (int)(dst - out);
}

/* --------------------------------------------------------------------------
 * clean_miio_sta_monitor_ntp_tick (@ 0x100ae84c)
 * Accumulates offline seconds and fires miio_ntp every 15s (0x0f).
 * -------------------------------------------------------------------------- */
bool clean_miio_sta_monitor_ntp_tick(miio_net_ctx_t *ctx, uint32_t elapsed_s)
{
    lock_net(ctx);
    uint32_t total = ctx->ntp_tick_s + elapsed_s;
    ctx->ntp_tick_s = (total < 15u) ? total : 0u;
    unlock_net(ctx);
    return total >= 15u;
}

/* --------------------------------------------------------------------------
 * clean_miio_sta_monitor_reset_offline (@ 0x100ae886)
 * Clears current STA offline duration accumulator upon cloud reconnect.
 * -------------------------------------------------------------------------- */
void clean_miio_sta_monitor_reset_offline(miio_net_ctx_t *ctx)
{
    lock_net(ctx);
    ctx->sta_offline_s = 0u;
    unlock_net(ctx);
}

/* --------------------------------------------------------------------------
 * clean_miio_sta_monitor_reset_history (@ 0x100ae8ae)
 * Resets sliding offline-flap window while connected/unprovisioned.
 * -------------------------------------------------------------------------- */
void clean_miio_sta_monitor_reset_history(miio_net_ctx_t *ctx)
{
    lock_net(ctx);
    ctx->offline_window_s = 0u;
    for (size_t i = 0; i < 10u; i++) {
        ctx->offline_history[i] = ctx->offline_count;
    }
    unlock_net(ctx);
}

/* --------------------------------------------------------------------------
 * clean_miio_net_state_change (@ 0x100ae8ec)
 * Updates global MiIO network state & invokes all registered callbacks.
 * -------------------------------------------------------------------------- */
void clean_miio_net_state_change(miio_net_ctx_t *ctx, uint32_t new_state, uint32_t err)
{
    bool changed = false;

    lock_net(ctx);
    if (err != 0u) {
        arch_dbg_log_begin("\x1b[0;31m");
        log_printf("[E] %s: miio net state change error [%d] (%s,%d)",
                   "miio_net", err, "miio_net_state_change", 0x47);
        arch_dbg_log_end("\x1b[0m");
    }
    if (g_clean_net_state != (uint8_t)new_state || g_clean_net_err != (uint8_t)err) {
        g_clean_net_state = (uint8_t)new_state;
        g_clean_net_err   = (uint8_t)err;
        changed = true;
    }
    unlock_net(ctx);

    if (changed && ctx->callbacks != NULL) {
        for (uint32_t i = 0; i < ctx->callback_count; i++) {
            if (ctx->callbacks[i].fn != NULL) {
                ctx->callbacks[i].fn(new_state, err, ctx->callbacks[i].ctx);
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * clean_sta_monitor_offline_timeout (@ 0x100aea08)
 * Checks if STA has been continuously offline >= sta_offline_timeout_s (900s default).
 *
 * [BUG #1] Stock firmware defaults to 900s (15 min) when 0 or 1 AP is known!
 *          Recovering from a transient router drop takes up to 15 minutes
 *          if the disconnect callback didn't immediately trigger a reconnect.
 * -------------------------------------------------------------------------- */
bool clean_sta_monitor_offline_timeout(miio_net_ctx_t *ctx, uint32_t elapsed_s)
{
    lock_net(ctx);
    uint32_t total = ctx->sta_offline_s + elapsed_s;
    uint32_t limit = ctx->sta_offline_timeout_s;
    ctx->sta_offline_s = total;
    if (limit <= total) {
        ctx->sta_offline_s = 0u;
        FUN_100f78f0(ctx->miio_handle, "miio_monitor", "offline timeout", 0x0fu);
        arch_dbg_log_begin("\x1b[0;33m");
        log_printf("[W] %s: sta offline timeout (%s,%d)",
                   "miio_net", "sta_monitor_offline_timeout", 0x3a4);
        arch_dbg_log_end("\x1b[0m");
    }
    unlock_net(ctx);
    return limit <= total;
}

/* --------------------------------------------------------------------------
 * clean_miio_wifi_scan_start (@ 0x100aedf0)
 * Clears AP cache, allocates scan table in N9/SDK driver, starts directed scan
 * for ctx->ssid, and arms the 2000ms scan timeout timer.
 * -------------------------------------------------------------------------- */
int clean_miio_wifi_scan_start(miio_net_ctx_t *ctx)
{
    int ret = 0;

    lock_net(ctx);
    if (ctx->net_started == 0u) {
        arch_dbg_log_begin("\x1b[0;33m");
        log_printf("[W] %s: net stoped (%s,%d)", "miio_net", "miio_wifi_scan_start", 0x219);
        arch_dbg_log_end("\x1b[0m");
        unlock_net(ctx);
        return 0;
    }

    clear_ap_list_locked(ctx);
    wifi_conn_scan_deinit();
    memset(g_clean_scan_table, 0, sizeof(g_clean_scan_table));

    ret = wifi_conn_scan_init(g_clean_scan_table, 20);
    if (ret < 0) {
        arch_dbg_log_begin("\x1b[0;31m");
        log_printf("[E] %s: wifi scanning init failed ret %d!!! (%s,%d)",
                   "miio_net", ret, "miio_wifi_scan_start", 0x230);
        arch_dbg_log_end("\x1b[0m");
    } else {
        uint8_t ssid_len = (uint8_t)strnlen(ctx->ssid, 0x21u);
        ret = wifi_conn_start_scan(ctx->ssid, ssid_len, NULL, 0, 0);
        if (ret != 0) {
            arch_dbg_log_begin("\x1b[0;31m");
            log_printf("[E] %s: wifi scanning start failed ret = [%d]!!! (%s,%d)",
                       "miio_net", ret, "miio_wifi_scan_start", 0x236);
            arch_dbg_log_end("\x1b[0m");
            wifi_conn_scan_deinit();
        }
    }

    if (ctx->scan_timer_created != 0x0bu) {
        ret = arch_os_timer_create(&ctx->scan_timer, "scanTimer", 2000u,
                                   (void *)(uintptr_t)clean_miio_wifi_scan_timeout,
                                   NULL, 0, 0);
        ctx->scan_timer_created = 0x0bu;
    } else {
        ret = arch_os_timer_activate(ctx->scan_timer);
    }

    unlock_net(ctx);
    return ret;
}

/* --------------------------------------------------------------------------
 * clean_miio_wifi_disconnect_clean (@ 0x100aef9c)
 * Disconnects STA and marks currently bound BSSID inactive in repeaters list.
 * -------------------------------------------------------------------------- */
void clean_miio_wifi_disconnect_clean(miio_net_ctx_t *ctx)
{
    lock_net(ctx);
    if (ctx->net_started != 0u) {
        if (ctx->disconnect_by_self == 0u) {
            wifi_conn_disconnect_ap();
            ctx->disconnect_by_self = 1u;
            for (miio_list_head_t *it = ctx->ap_list.next; it != &ctx->ap_list; it = it->next) {
                miio_wifi_ap_entry_t *ap = (miio_wifi_ap_entry_t *)it;
                if (memcmp(ctx->current_ap.bssid, ap->bssid, 6u) == 0) {
                    ap->active = 0u;
                    break;
                }
            }
        }
        ctx->wifi_state = MIIO_WIFI_STA_IDLE;
    }
    unlock_net(ctx);
}

/* --------------------------------------------------------------------------
 * clean_miio_wifi_connect (@ 0x100af028)
 * Picks strongest active AP from ap_list, pushes country/SSID/PSK/BSSID/channel
 * to MTK Wi-Fi Service, and calls wifi_config_reload_setting() to trigger auth.
 *
 * [BUG #2] Pins exact BSSID + primary channel (`set_bssid` + `set_channel`)!
 *          If router changes channel (Auto/DFS) or AP mesh steers client,
 *          reassociation fails repeatedly until a full rescan occurs.
 * [BUG #3] `wifi_config_set_bssid` in SDK prints raw 6-byte binary MAC with `%s`,
 *          and `wifi_config_reload_setting` leaks its wait semaphore if inband fails.
 * -------------------------------------------------------------------------- */
int clean_miio_wifi_connect(miio_net_ctx_t *ctx)
{
    miio_wifi_ap_entry_t *best = NULL;
    char mac_str[18];
    uint8_t cc_buf[12];
    int ret;

    lock_net(ctx);
    if (ctx->net_started == 0u) {
        unlock_net(ctx);
        return 0;
    }

    for (miio_list_head_t *it = ctx->ap_list.next; it != &ctx->ap_list; it = it->next) {
        miio_wifi_ap_entry_t *ap = (miio_wifi_ap_entry_t *)it;
        if (ap->active == 0u) {
            continue;
        }
        if (best == NULL || best->rssi < ap->rssi) {
            best = ap;
        }
    }

    if (best == NULL) {
        arch_dbg_log_begin("\x1b[0;31m");
        log_printf("[E] %s: connect error: no ap info (%s,%d)", "miio_net", "miio_wifi_connect", 0x1ac);
        arch_dbg_log_end("\x1b[0m");
        unlock_net(ctx);
        return -9; /* 0xfffffff7 */
    }

    FUN_100a7ab8(mac_str, sizeof(mac_str), best->bssid, 6u, ':');
    arch_dbg_log_begin("\x1b[0;32m");
    log_printf("[I] %s: bingo ssid=%s, bssid=%s, pri=%d, rssi=%d",
               "miio_net", best->ssid, mac_str, best->channel, (int)best->rssi);
    arch_dbg_log_end("\x1b[0m");

    memcpy(&ctx->current_ap, &best->ssid, sizeof(ctx->current_ap));

    memset(cc_buf, 0, sizeof(cc_buf));
    strncpy((char *)cc_buf, ctx->country_code, 3u);
    cc_buf[5] = 1u;
    cc_buf[6] = ctx->schan;
    cc_buf[7] = ctx->nchan;
    cc_buf[8] = ctx->max_tx_dbm;
    cc_buf[10] = 0u;
    (void)wifi_config_set_country(cc_buf);

    uint8_t ssid_len = (uint8_t)strnlen(ctx->ssid, 0x21u);
    uint8_t psk_len  = (uint8_t)strnlen(ctx->password, 0x41u);
    int r_ssid = wifi_config_set_ssid(0u, ctx->ssid, ssid_len);
    int r_psk  = (psk_len != 0u) ? wifi_config_set_psk(0u, ctx->password, psk_len) : 0;
    int r_bss  = wifi_config_set_bssid(ctx->current_ap.bssid);
    int r_ch   = wifi_config_set_channel(0u, ctx->current_ap.channel);
    if ((r_ssid | r_psk | r_bss | r_ch) < 0) {
        arch_dbg_log_begin("\x1b[0;31m");
        log_printf("[E] %s: set bssid or channel failed! (%s,%d)",
                   "miio_net", "miio_wifi_connect", 0x1d2);
        arch_dbg_log_end("\x1b[0m");
    }

    if (wifi_config_reload_setting() < 0) {
        ctx->wifi_state = MIIO_WIFI_STA_IDLE;
        ret = -12; /* 0xfffffff4 */
    } else {
        ctx->wifi_state = MIIO_WIFI_STA_CONNECTING;
        ctx->connect_count++;
        ctx->disconnect_by_self = 0u;
        ret = 0;
    }

    unlock_net(ctx);
    return ret;
}

/* --------------------------------------------------------------------------
 * clean_miio_wifi_try_reconnect (@ 0x100af244)
 *
 * [BUG #4 — CRITICAL] In stock firmware (0x100af298: bls #0x100af2b2):
 *   If >= 2 active APs (repeaters/mesh) are cached, it returns 0 WITHOUT
 *   triggering connect, scan, or caller's stop+restart path! On timeout/countout,
 *   the caller trusts return code 0 and does NOTHING, leaving the lamp stuck.
 *   Patch #2 fixes 0x100af298 (bls -> b) so caller always performs clean restart.
 * -------------------------------------------------------------------------- */
int clean_miio_wifi_try_reconnect(miio_net_ctx_t *ctx)
{
    int ret;

    lock_net(ctx);
    if (ctx->net_started == 0u) {
        unlock_net(ctx);
        return 0;
    }

    wifi_conn_disconnect_ap();
    uint32_t active = count_active_aps_locked(ctx);

    if (active < 2u) {
        ret = -8; /* Caller executes miio_net_stop() + miio_net_start_async(5000) */
    } else {
        /* [BUG #4] Stock returns 0 here without reconnecting or scanning! */
        arch_dbg_log_begin("\x1b[0;32m");
        log_printf("[I] %s: Wifi station reconnecting.", "miio_net");
        arch_dbg_log_end("\x1b[0m");
        ret = 0;
    }

    unlock_net(ctx);
    return ret;
}

/* --------------------------------------------------------------------------
 * clean_miio_net_post_event (@ 0x100af2e0)
 * Enqueues an event onto netMonitorTask's queue (blocking wait).
 * -------------------------------------------------------------------------- */
void clean_miio_net_post_event(miio_wifi_event_id_t evt)
{
    miio_wifi_msg_t msg = { .event = (uint8_t)evt };
    arch_os_queue_send(g_clean_miio_net_ctx.monitor_queue, &msg, 0xffffffffu);
}

/* --------------------------------------------------------------------------
 * clean_miio_wifi_scan_timeout (@ 0x100af334)
 * Timer expiry fallback when N9 scan-complete IRQ/event never arrives (2s).
 * -------------------------------------------------------------------------- */
uint32_t clean_miio_wifi_scan_timeout(void)
{
    clean_miio_net_post_event(MIIO_WIFI_EVENT_SCAN_DONE);
    return 0;
}

/* --------------------------------------------------------------------------
 * clean_wifi_cb_event_handler (@ 0x100af370)
 * MTK Wi-Fi driver async callback (runs in wifi/net task context).
 *
 * [BUG #5 — CRITICAL] Case WIFI_EVT_IOT_CONNECT_FAILED (8):
 *   On association/4-way handshake timeout or rejection, it ONLY sets state to
 *   MIIO_NET_STATE_DISCONNECTED and clears miio_mode to 0 — it NEVER posts
 *   MIIO_WIFI_EVENT_STA_DISCONNECTED (5)! Lamp stays stalled up to 900s!
 * [BUG #6] Case WIFI_EVT_IOT_DISCONNECTED (2):
 *   Clears lwIP IP address (`netif_set_addr(st2, 0, 0, 0)`) on any brief
 *   deauth/roam, forcing full DHCP discovery on reconnect.
 * -------------------------------------------------------------------------- */
uint32_t clean_wifi_cb_event_handler(uint32_t event_id, const uint8_t *payload)
{
    uint8_t opmode = 0;

    switch (event_id) {
    case WIFI_EVT_IOT_CONNECTED:
        (void)wifi_config_get_opmode(&opmode);
        if (opmode == WIFI_OPMODE_STA) {
            clean_miio_net_post_event(MIIO_WIFI_EVENT_STA_CONNECTED);
        }
        return 0;

    case WIFI_EVT_IOT_SCAN_COMPLETE:
        clean_miio_net_post_event(MIIO_WIFI_EVENT_SCAN_DONE);
        return 0;

    case WIFI_EVT_IOT_DISCONNECTED: {
        (void)wifi_config_get_opmode(&opmode);
        if (opmode != WIFI_OPMODE_STA) {
            return 0;
        }
        uint8_t zero_mac[6] = {0};
        if (memcmp(payload, zero_mac, 6u) == 0) {
            return 0;
        }
        FUN_100f78b0(g_clean_miio_net_ctx.miio_handle, 0u);
        clean_miio_net_state_change(&g_clean_miio_net_ctx, MIIO_NET_STATE_DISCONNECTED, 0u);
        if (g_clean_miio_net_ctx.disconnect_by_self != 0u) {
            return 0;
        }
        void *st2 = lwip_netif_find_idx(2);
        lwip_netif_set_link_down(st2);
        if (FUN_100ba16c() == 1) {
            lwip_netif_set_addr(st2, &g_zero_ip4, &g_zero_ip4, &g_zero_ip4);
        }
        clean_miio_net_post_event(MIIO_WIFI_EVENT_STA_DISCONNECTED);
        return 0;
    }

    case WIFI_EVT_IOT_INIT_COMPLETE:
        (void)wifi_config_get_opmode(&opmode);
        if (g_clean_miio_net_ctx.target_opmode == WIFI_OPMODE_STA) {
            clean_miio_net_post_event(MIIO_WIFI_EVENT_STA_START);
        } else if (g_clean_miio_net_ctx.target_opmode == WIFI_OPMODE_AP) {
            clean_miio_net_post_event(MIIO_WIFI_EVENT_AP_START);
        }
        g_clean_miio_net_ctx.wifi_init_done = 1u;
        return 0;

    case WIFI_EVT_IOT_CONNECT_FAILED: {
        /* payload[0] = port, payload[1] = reason ('d' == 100 password error) */
        uint32_t err = (payload[1] == 100u) ? 1u : 2u;
        clean_miio_net_state_change(&g_clean_miio_net_ctx, MIIO_NET_STATE_DISCONNECTED, err);
        FUN_100f78b0(g_clean_miio_net_ctx.miio_handle, 0u);
        /* [BUG #5] Missing: clean_miio_net_post_event(MIIO_WIFI_EVENT_STA_DISCONNECTED); */
        return 0;
    }

    default:
        return 0;
    }
}

/* --------------------------------------------------------------------------
 * clean_ip_ready_callback (@ 0x100af73c)
 * Called by lwIP when DHCP succeeds or expires (`st2->ip_addr` changes).
 * -------------------------------------------------------------------------- */
void clean_ip_ready_callback(const void *netif_ptr)
{
    const uint32_t *words = (const uint32_t *)netif_ptr;
    const uint8_t  *ip_ptr = (const uint8_t *)&words[1];
    uint8_t opmode = 0;

    if (words[1] == 0u) {
        return;
    }

    const char *ip_str = lwip_ip4_ntoa(ip_ptr);
    if (ip_str == NULL) {
        FUN_100f78b0(g_clean_miio_net_ctx.miio_handle, 0u);
        clean_miio_net_post_event(MIIO_WIFI_EVENT_STA_LOST_IP);
    } else {
        xQueueGenericSend(g_clean_sta_sem, NULL, 0u, 0);
        uint32_t mode = (g_clean_miio_net_ctx.factory_mode == 0u) ? 2u : 1u;
        FUN_100f78b0(g_clean_miio_net_ctx.miio_handle, mode);
        clean_miio_net_state_change(&g_clean_miio_net_ctx, MIIO_NET_STATE_LOCAL, 0u);
        clean_miio_net_post_event(MIIO_WIFI_EVENT_STA_GOT_IP);
        FUN_100bf038();
    }

    void *ap1 = lwip_netif_find_idx(1);
    if (wifi_config_get_opmode(&opmode) >= 0 && opmode == WIFI_OPMODE_REP) {
        lwip_netif_set_addr(ap1, ip_ptr, &words[2], &words[3]);
    }
}

/* --------------------------------------------------------------------------
 * clean_miio_net_get_wifi_state (@ 0x100afc14)
 * Returns live high-level MIIO_NET_STATE_* derived from ot/ota/st2 IP.
 *
 * [BUG #7] Reports MIIO_NET_STATE_LOCAL (3) solely because st2->ip != 0 and
 *          ot != online — never verifies L2 link or gateway ARP/ICMP!
 * -------------------------------------------------------------------------- */
uint8_t clean_miio_net_get_wifi_state(void)
{
    if (g_clean_miio_net_ctx.provisioned == 0u) {
        uint8_t opmode = 0;
        if (wifi_config_get_opmode(&opmode) < 0) {
            return MIIO_NET_STATE_UNPROV;
        }
        return (opmode == WIFI_OPMODE_AP) ? MIIO_NET_STATE_UAP : MIIO_NET_STATE_UNPROV;
    }
    if (FUN_100f8a78(g_clean_miio_net_ctx.miio_handle)) {
        return MIIO_NET_STATE_UPDATING;
    }
    if (FUN_100f9f04(g_clean_miio_net_ctx.miio_handle) != 0u) {
        return MIIO_NET_STATE_CLOUD;
    }
    return (FUN_100a850c() == 0) ? MIIO_NET_STATE_LOCAL : MIIO_NET_STATE_DISCONNECTED;
}

/* --------------------------------------------------------------------------
 * clean_lwip_net_stop (@ 0x100b05a4) & clean_lwip_net_start (@ 0x100affa8)
 * -------------------------------------------------------------------------- */
void clean_lwip_net_stop(int opmode)
{
    void *st2 = lwip_netif_find_idx(2);
    void *ap1 = lwip_netif_find_idx(1);
    if (opmode == WIFI_OPMODE_AP) {
        FUN_1009b178();
        lwip_netif_set_link_down(ap1);
    } else if (opmode == WIFI_OPMODE_STA || opmode == WIFI_OPMODE_REP) {
        lwip_netif_set_status_cb(st2, NULL);
        lwip_dhcp_stop(st2);
        lwip_dhcp_release(st2);
        lwip_netif_set_link_down(st2);
    }
}

void clean_lwip_net_start(int opmode)
{
    uint8_t tcpip_cfg[28];
    uint8_t dhcpd_cfg[112];

    if (opmode == WIFI_OPMODE_AP) {
        memset(dhcpd_cfg, 0, sizeof(dhcpd_cfg));
        memset(tcpip_cfg, 0, sizeof(tcpip_cfg));
        if (FUN_100ba1ac(tcpip_cfg) == 0) {
            FUN_100ba248(tcpip_cfg, dhcpd_cfg);
            void *ap1 = lwip_netif_find_idx(1);
            lwip_netif_set_up(ap1);
            lwip_netif_set_link_up(ap1);
            FUN_1009aea4(dhcpd_cfg);
        }
    } else if (opmode == WIFI_OPMODE_STA || opmode == WIFI_OPMODE_REP) {
        if (FUN_100ba16c() == 1) {
            uint32_t *st2 = (uint32_t *)lwip_netif_find_idx(2);
            st2[1] = 0u;
            st2[2] = 0u;
            st2[3] = 0u;
            lwip_netif_set_up(st2);
            lwip_netif_set_status_cb(st2, clean_ip_ready_callback);
            lwip_dhcp_start(st2);
        }
    }
}

/* --------------------------------------------------------------------------
 * clean_wifi_set_opmode (@ 0x100b0704)
 * -------------------------------------------------------------------------- */
int clean_wifi_set_opmode(uint8_t opmode)
{
    uint8_t cur = 0;
    (void)wifi_config_get_opmode(&cur);
    if (cur == opmode) {
        return 0;
    }
    clean_lwip_net_stop(cur);
    if (wifi_config_set_opmode(opmode) < 0) {
        return 0xff;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * clean_miio_net_stop (@ 0x100b0610)
 * Shuts down netMonitor tracking, disconnects STA / disables radio from AP.
 * -------------------------------------------------------------------------- */
int clean_miio_net_stop(void)
{
    miio_net_ctx_t *ctx = &g_clean_miio_net_ctx;
    uint8_t opmode = 0;

    lock_net(ctx);
    ctx->monitor_enabled = 0u;
    ctx->ap_timeout_s = 0u;
    ctx->sta_offline_s = 0u;
    ctx->sta_offline_timeout_s = 900u;
    ctx->offline_window_s = 0u;
    ctx->offline_count = 0u;
    for (size_t i = 0; i < 10u; i++) {
        ctx->offline_history[i] = 0u;
    }
    ctx->offline_count_threshold = 10u;
    ctx->ever_online = 1u;
    ctx->ntp_tick_s = 0u;
    ctx->net_started = 0u;
    ctx->wifi_state = MIIO_WIFI_STA_IDLE;

    (void)wifi_config_get_opmode(&opmode);
    if (opmode == WIFI_OPMODE_STA) {
        if (ctx->disconnect_by_self == 0u) {
            wifi_conn_disconnect_ap();
            ctx->disconnect_by_self = 1u;
        }
        clean_miio_net_post_event(MIIO_WIFI_EVENT_STA_STOP);
    } else if (opmode == WIFI_OPMODE_AP && ctx->provisioned == 0u) {
        clean_lwip_net_stop(WIFI_OPMODE_AP);
        wifi_config_set_opmode(WIFI_OPMODE_STA);
        wifi_config_reload_setting();
        wifi_config_set_radio(0u);
        clean_miio_net_post_event(MIIO_WIFI_EVENT_AP_STOP);
    }
    clean_lwip_net_stop(opmode);
    unlock_net(ctx);
    return 0;
}

/* --------------------------------------------------------------------------
 * clean_miio_net_start (@ 0x100b0788)
 * Re-applies country code, STA credentials, opmode, lwIP DHCP, and kicks scan.
 * -------------------------------------------------------------------------- */
int clean_miio_net_start(void)
{
    miio_net_ctx_t *ctx = &g_clean_miio_net_ctx;
    uint8_t cc_cfg[16];

    lock_net(ctx);
    memset(cc_cfg, 0, sizeof(cc_cfg));
    strncpy((char *)cc_cfg, ctx->country_code, 3u);
    cc_cfg[5] = 1u;
    cc_cfg[6] = ctx->schan;
    cc_cfg[7] = ctx->nchan;
    cc_cfg[8] = ctx->max_tx_dbm;
    (void)wifi_config_set_country(cc_cfg);

    if (ctx->provisioned != 0u) {
        ctx->target_opmode = WIFI_OPMODE_STA;
        if (ctx->factory_mode == 0u) {
            FUN_100f9768(ctx->miio_handle);
        } else {
            FUN_100f9740(ctx->miio_handle);
        }
        uint8_t ssid_len = (uint8_t)strnlen(ctx->ssid, 0x21u);
        uint8_t psk_len  = (uint8_t)strnlen(ctx->password, 0x41u);
        int rc = clean_wifi_set_opmode(ctx->target_opmode);
        rc |= wifi_config_set_ssid(0u, ctx->ssid, ssid_len);
        if (psk_len != 0u) {
            rc |= wifi_config_set_psk(0u, ctx->password, psk_len);
        }
        if (rc < 0) {
            unlock_net(ctx);
            return -1;
        }
    }

    clean_lwip_net_start(ctx->target_opmode);
    if (ctx->target_opmode == WIFI_OPMODE_STA) {
        clean_miio_net_post_event(MIIO_WIFI_EVENT_STA_START);
    } else if (ctx->target_opmode == WIFI_OPMODE_AP) {
        clean_miio_net_post_event(MIIO_WIFI_EVENT_AP_START);
    }

    ctx->monitor_enabled = 1u;
    ctx->ap_timeout_s = 0u;
    ctx->sta_offline_s = 0u;
    ctx->sta_offline_timeout_s = 900u;
    ctx->offline_window_s = 0u;
    ctx->offline_count = 0u;
    for (size_t i = 0; i < 10u; i++) {
        ctx->offline_history[i] = 0u;
    }
    ctx->offline_count_threshold = 10u;
    ctx->ever_online = 1u;
    ctx->ntp_tick_s = 0u;
    ctx->net_started = 1u;
    ctx->disconnect_by_self = 0u;
    unlock_net(ctx);
    return 0;
}

/* --------------------------------------------------------------------------
 * clean_sta_monitor_offline_countout (inlined inside 0x100b0a08)
 * Sliding 10-bucket detector for flapping Wi-Fi (`>= threshold` offline events).
 * -------------------------------------------------------------------------- */
bool clean_sta_monitor_offline_countout(miio_net_ctx_t *ctx, uint32_t elapsed_s)
{
    bool tripped = false;

    lock_net(ctx);
    ctx->offline_window_s += elapsed_s;
    uint32_t bucket_s = ctx->sta_offline_timeout_s / 10u;
    if (bucket_s == 0u) {
        bucket_s = 1u;
    }
    if (ctx->offline_window_s >= bucket_s) {
        ctx->offline_window_s = 0u;
        for (size_t i = 0; i < 9u; i++) {
            ctx->offline_history[i] = ctx->offline_history[i + 1u];
        }
        ctx->offline_history[9] = ctx->offline_count;
        if ((uint32_t)(ctx->offline_count - ctx->offline_history[0]) >= ctx->offline_count_threshold) {
            FUN_100f78f0(ctx->miio_handle, "miio_monitor", "offline countout", 0x10u);
            tripped = true;
        }
    }
    unlock_net(ctx);
    return tripped;
}

/* --------------------------------------------------------------------------
 * clean_miio_wifi_scan_finish (@ 0x100b0a08)
 * Main `netMonitorTask` loop: handles queue events 0..9 + 1-second health checks.
 *
 * [BUG #8] When miio_get_mode() == 1 (MIIO_NET_LOCAL), the monitor NEVER calls
 *          sta_monitor_offline_timeout() and NEVER pings the gateway — if the
 *          router drops STA silently with cached IP, Wi-Fi stays dead forever.
 * [BUG #9] On SCAN_DONE with 1 AP: sta_offline_timeout_s = 900s (15 minutes).
 * -------------------------------------------------------------------------- */
static void process_scan_done_locked(miio_net_ctx_t *ctx)
{
    if (ctx->net_started == 0u) {
        return;
    }
    arch_os_timer_deactivate(ctx->scan_timer);
    wifi_conn_scan_deinit();

    size_t target_len = strnlen(ctx->ssid, 0x20u);
    for (size_t i = 0; i < 20u; i++) {
        mtk_scan_ap_t *slot = &g_clean_scan_table[i];
        if (slot->valid == 0u) {
            continue;
        }
        if (slot->auth_mode == 0u && ctx->password[0] != '\0') {
            continue;
        }
        size_t cand_len = strnlen(slot->ssid, 0x20u);
        if (cand_len != target_len || strncmp(slot->ssid, ctx->ssid, cand_len) != 0) {
            continue;
        }
        miio_wifi_ap_entry_t *entry = (miio_wifi_ap_entry_t *)pvPortCalloc(1u, sizeof(*entry));
        if (entry == NULL) {
            continue;
        }
        entry->node.next = ctx->ap_list.next;
        entry->node.prev = &ctx->ap_list;
        ctx->ap_list.next->prev = &entry->node;
        ctx->ap_list.next = &entry->node;

        entry->ssid      = slot->ssid;
        memcpy(entry->bssid, slot->bssid, 6u);
        entry->channel   = slot->channel;
        entry->rssi      = slot->rssi;
        entry->auth_mode = slot->auth_mode;
        entry->enc_type  = slot->enc_type;
        entry->active    = 1u;
        ctx->ap_count++;

        if (ctx->ap_count > 4u) {
            /* Keep top 4 strongest APs by RSSI */
            miio_wifi_ap_entry_t *worst = NULL;
            for (miio_list_head_t *it = ctx->ap_list.next; it != &ctx->ap_list; it = it->next) {
                miio_wifi_ap_entry_t *cur = (miio_wifi_ap_entry_t *)it;
                if (worst == NULL || (worst->active != 0u && cur->active == 0u) ||
                    cur->rssi < worst->rssi) {
                    worst = cur;
                }
            }
            if (worst != NULL) {
                worst->node.next->prev = worst->node.prev;
                worst->node.prev->next = worst->node.next;
                vPortFree(worst);
                ctx->ap_count--;
            }
        }
    }

    wifi_conn_stop_scan();
    uint32_t active = count_active_aps_locked(ctx);
    ctx->wifi_state = MIIO_WIFI_STA_SCANNED;
    ctx->ap_count = active;

    if (active == 0u) {
        uint32_t next_ms = ctx->scan_interval_ms * 2u;
        ctx->scan_interval_ms = (next_ms < 200u) ? 200u : (next_ms > 1000u ? 1000u : next_ms);
        ctx->sta_offline_timeout_s = 900u;
        ctx->offline_count_threshold = 10u;
    } else {
        uint32_t next_ms = ctx->scan_interval_ms;
        if (next_ms < 400u) {
            next_ms = 200u;
        } else if (next_ms <= 2001u) {
            next_ms >>= 1;
        } else {
            next_ms = 1000u;
        }
        ctx->scan_interval_ms = next_ms;
        ctx->sta_offline_timeout_s = 900u / active;
        ctx->offline_count_threshold = (ctx->sta_offline_timeout_s * 1000u) / 90000u;
    }
}

void clean_miio_wifi_scan_finish(miio_net_ctx_t *ctx)
{
    miio_wifi_msg_t msg;
    uint32_t elapsed_ms = (uint32_t)arch_os_ms_elapsed(ctx->last_tick_ms);
    uint32_t elapsed_s = elapsed_ms / 1000u;

    if (arch_os_queue_recv(ctx->monitor_queue, &msg, 1000u) == 0) {
        switch (msg.event) {
        case MIIO_WIFI_EVENT_CONFIG_ROUTER:
            (void)clean_miio_net_start();
            break;
        case MIIO_WIFI_EVENT_SCAN_DONE:
            lock_net(ctx);
            process_scan_done_locked(ctx);
            unlock_net(ctx);
            if (clean_miio_wifi_connect(ctx) != 0) {
                (void)clean_miio_wifi_scan_start(ctx);
            }
            break;
        case MIIO_WIFI_EVENT_STA_START:
            (void)clean_miio_wifi_scan_start(ctx);
            break;
        case MIIO_WIFI_EVENT_STA_STOP:
            FUN_100f78b0(ctx->miio_handle, 0u);
            break;
        case MIIO_WIFI_EVENT_STA_CONNECTED:
            ctx->wifi_state = MIIO_WIFI_STA_CONNECTED;
            break;
        case MIIO_WIFI_EVENT_STA_DISCONNECTED:
            clean_miio_wifi_disconnect_clean(ctx);
            if (clean_miio_wifi_connect(ctx) != 0) {
                (void)clean_miio_wifi_scan_start(ctx);
            }
            break;
        case MIIO_WIFI_EVENT_STA_GOT_IP:
            ctx->wifi_state = MIIO_WIFI_STA_GOT_IP;
            break;
        case MIIO_WIFI_EVENT_STA_LOST_IP:
            FUN_100f78f0(ctx->miio_handle, "miio_monitor", "dhcp failed", 0x0bu);
            (void)clean_miio_net_stop();
            (void)clean_miio_net_start();
            break;
        case MIIO_WIFI_EVENT_AP_START:
            FUN_100f78b0(ctx->miio_handle, 1u);
            clean_miio_net_state_change(ctx, MIIO_NET_STATE_UAP, 0u);
            break;
        case MIIO_WIFI_EVENT_AP_STOP:
            FUN_100f78b0(ctx->miio_handle, 0u);
            clean_miio_net_state_change(ctx, MIIO_NET_STATE_UNPROV, 0u);
            break;
        default:
            break;
        }
    }

    if (elapsed_s == 0u) {
        return;
    }

    ctx->last_tick_ms = arch_os_ms_now();
    ctx->work_time_s += elapsed_s;
    (void)xPortGetFreeHeapSize();

    if (ctx->monitor_enabled == 0u) {
        ctx->time_unprov_s += elapsed_s;
        return;
    }

    uint8_t mode = FUN_100f72dc(ctx->miio_handle);
    if (mode == 2u) {
        if (FUN_100f9f04(ctx->miio_handle) == 0u) {
            if (clean_miio_sta_monitor_ntp_tick(ctx, elapsed_s)) {
                (void)miio_ntp(ctx->miio_handle, NULL, ctx, 7000u);
            }
            if (clean_sta_monitor_offline_timeout(ctx, elapsed_s)) {
                if (clean_miio_wifi_try_reconnect(ctx) != 0) {
                    (void)clean_miio_net_stop();
                    (void)clean_miio_net_start();
                }
            }
        } else {
            ctx->time_online_s += elapsed_s;
            clean_miio_sta_monitor_reset_offline(ctx);
        }
        if (clean_sta_monitor_offline_countout(ctx, elapsed_s)) {
            if (clean_miio_wifi_try_reconnect(ctx) != 0) {
                (void)clean_miio_net_stop();
                (void)clean_miio_net_start();
            }
        }
    } else if (mode == 1u) {
        /* [BUG #8] Local mode only refreshes NTP & clears counters, zero dead-link recovery! */
        if (clean_miio_sta_monitor_ntp_tick(ctx, elapsed_s)) {
            (void)miio_ntp(ctx->miio_handle, NULL, ctx, 7000u);
        }
        clean_miio_sta_monitor_reset_offline(ctx);
        clean_miio_sta_monitor_reset_history(ctx);
    }

    clean_miio_net_state_change(ctx, clean_miio_net_get_wifi_state(), 0u);
}

/* --------------------------------------------------------------------------
 * clean_yglc_network_monitor (@ 0x100bd918)
 * Application-level 5-minute dead-connection reboot guard.
 *
 * [BUG #10] Stock binary checks `g_yglc_settings.power == YGLC_POWER_OFF`
 *           (0x100bd942: bne), skipping reboot whenever the lamp is turned ON.
 *           Patch #1 replaces bne -> nop so 5-min watchdog works in all states.
 * -------------------------------------------------------------------------- */
void clean_yglc_network_monitor(void)
{
    static uint32_t offline_since_ms = 0u;
    uint8_t state = clean_miio_net_get_wifi_state();

    if (state == MIIO_NET_STATE_DISCONNECTED) {
        if (offline_since_ms == 0u) {
            offline_since_ms = arch_os_ms_now();
            return;
        }
        /* Fixed vs stock: no power == YGLC_POWER_OFF gate here! */
        uint32_t elapsed = (uint32_t)(arch_os_ms_now() - offline_since_ms);
        if (elapsed > 300000u) {
            miio_reboot((uint32_t)(uintptr_t)g_clean_miio_net_ctx.miio_handle, "cmd.reboot");
        }
        return;
    }

    offline_since_ms = arch_os_ms_now();
}
