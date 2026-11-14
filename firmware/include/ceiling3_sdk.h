#pragma once
/*
 * Yeelight Ceiling3 (yeelink.light.ceiling3, 2.0.2_0048) Cortex-M4F Firmware SDK.
 * Verified function signatures & SRAM globals mapped from Ghidra/binary analysis.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../../decomp/include/fw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- MiIO network states (miio_net_get_state) ---- */
typedef enum {
    MIIO_NET_UNPROV       = 0,
    MIIO_NET_UAP          = 1,
    MIIO_NET_DISCONNECTED = 2,
    MIIO_NET_LOCAL        = 3,
    MIIO_NET_CLOUD        = 4,
    MIIO_NET_UPDATING     = 5,
} miio_net_state_t;

/* ---- Light power state inside yglc_settings_t->power (+0x54) ---- */
#define YGLC_POWER_ON  0u
#define YGLC_POWER_OFF 1u

/* ---- lwIP ARP table entry (0x1c = 28 bytes @ 0x2000264c, 10 slots) ---- */
typedef struct {
    void    *q;
    uint32_t ipaddr;
    void    *netif;
    uint8_t  ethaddr[6];
    uint16_t ctime;
    uint8_t  state;   /* 0=EMPTY, 1=PENDING, 2+=STABLE */
    uint8_t  _pad[3];
    uint32_t ts;      /* xTaskGetTickCount() / 1000 on last ARP reply */
} ceiling3_arp_entry_t;

#ifdef HOST_TEST
/* Host unit-test environment injects mock globals & functions cleanly. */
extern int32_t g_log_level;
extern miio_net_t g_miio_net;
extern yglc_settings_t *g_yglc_settings_ptr;
extern uint64_t g_offline_since_ms;
extern uint8_t g_mock_st2_netif[64];
extern uint32_t g_mock_tcpip_lock;
extern ceiling3_arp_entry_t g_mock_arp_table[10];
#define g_yglc_settings (*g_yglc_settings_ptr)
#define CEILING3_ARP_TABLE_PTR  g_mock_arp_table
#define CEILING3_TCPIP_LOCK_PTR (&g_mock_tcpip_lock)
#define CEILING3_CMD_REBOOT_STR "cmd.reboot"
#define CEILING3_MONITOR_STATE_PTR ((volatile uint32_t *)&g_offline_since_ms)
#define CEILING3_MIIO_HANDLE    ((uint32_t)(uintptr_t)g_miio_net.miio_handle)
#else
#include "../../decomp/include/fw_globals.h"
#define CEILING3_ARP_TABLE_PTR  ((ceiling3_arp_entry_t *)0x2000264cu)
#define CEILING3_TCPIP_LOCK_PTR ((void *)0x20002c40u)
#define CEILING3_CMD_REBOOT_STR ((const char *)0x101241b1u)
#define CEILING3_MONITOR_STATE_PTR ((volatile uint32_t *)0x20008030u)
#define CEILING3_MIIO_HANDLE    (*(volatile uint32_t *)0x20004b24u)
#endif

/* ---- Verified firmware ROM symbols (linked via ceiling3_symbols.ld) ---- */
uint32_t arch_os_ms_now(void);
int arch_os_ms_elapsed(uint32_t since_ms);
int miio_net_get_state(void);
uint8_t FUN_100afc14(void); /* miio_net_get_wifi_state */
uint8_t *FUN_10083558(int idx); /* lwip netif_by_index(2 = st2) */
void FUN_10083808(void *mutex); /* sys_mutex_lock(lock_tcpip_core) */
void FUN_1008381c(void *mutex); /* sys_mutex_unlock(lock_tcpip_core) */
void FUN_1007d658(void *netif, void *ipaddr); /* etharp_request */
void miio_reboot(uint32_t miio_handle, const char *reason);
void yglc_reboot_save(int mode);
void hal_sys_reboot(int mode);
void hal_wdt_software_reset(void);
int miio_wifi_try_reconnect(void *ctx);
int arch_ota_check_crc(const arch_ota_part_t *part, uint32_t size);

void arch_dbg_log_begin(const char *color_esc);
void arch_dbg_log_end(const char *reset_esc);
int log_printf(const char *fmt, ...);

/* ---- Custom C extension exports (implemented in firmware/src/) ---- */
typedef struct {
    uint32_t disconnect_reboot_ms;   /* default 300000 ms (5 min) */
    uint32_t local_stale_reboot_ms;  /* 0 = disabled, e.g. 900000 ms (15 min) */
    uint8_t reboot_even_when_on;     /* non-zero: patch #1 behavior */
    uint8_t _reserved[3];
} ceiling3_guard_cfg_t;

typedef struct {
    uint32_t local_since_ms;
    uint8_t local_tracking;
    uint8_t reboot_triggered;
    uint16_t reboot_count;
} ceiling3_guard_state_t;

void ceiling3_guard_init(ceiling3_guard_state_t *st);
int ceiling3_network_monitor_step(
    ceiling3_guard_state_t *st,
    const ceiling3_guard_cfg_t *cfg,
    bool local_link_healthy
);
void ceiling3_network_monitor_hook(void);
void ceiling3_yglc_network_monitor_inplace(void);
int ceiling3_should_force_wifi_restart(int ap_count);
int ceiling3_is_ota_rpc_allowed(const miio_rpc_delegate_arg_t *req);
int ceiling3_verify_ota_image(const ota_app_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#if defined(__arm__)
_Static_assert(sizeof(ceiling3_arp_entry_t) == 0x1c, "arp_entry size 0x1c");
_Static_assert(offsetof(ceiling3_arp_entry_t, ipaddr) == 0x04, "arp_entry ipaddr");
_Static_assert(offsetof(ceiling3_arp_entry_t, state) == 0x14, "arp_entry state");
_Static_assert(offsetof(ceiling3_arp_entry_t, ts) == 0x18, "arp_entry ts");
_Static_assert(offsetof(miio_rpc_delegate_arg_t, channel_flags) == 0x08, "miio_rpc channel_flags");
_Static_assert(offsetof(miio_rpc_delegate_arg_t, pload_len) == 0x0c, "miio_rpc pload_len");
_Static_assert(offsetof(miio_rpc_delegate_arg_t, pload) == 0x10, "miio_rpc pload");
_Static_assert(offsetof(arch_ota_part_t, flash_offset) == 0x14, "arch_ota flash_offset");
_Static_assert(offsetof(arch_ota_part_t, partition_len) == 0x18, "arch_ota partition_len");
_Static_assert(offsetof(ota_app_ctx_t, signed_file) == 0x38, "ota_app signed_file");
_Static_assert(offsetof(ota_app_ctx_t, original_length) == 0x3c, "ota_app original_length");
_Static_assert(offsetof(miio_net_t, provisioned) == 0x2c, "miio_net provisioned");
_Static_assert(offsetof(miio_net_t, sta_state) == 0x3c, "miio_net sta_state");
_Static_assert(offsetof(miio_net_t, scan_list_next) == 0xc0, "miio_net scan_list_next");
_Static_assert(offsetof(miio_net_t, monitor_queue) == 0x154, "miio_net monitor_queue");
_Static_assert(offsetof(miio_net_t, wifi_started) == 0x1c8, "miio_net wifi_started");
_Static_assert(offsetof(yglc_settings_t, power) == 0x54, "yglc_settings power");
_Static_assert(offsetof(yglc_settings_t, init_power_opt) == 0x100, "yglc_settings init_power_opt");
#endif
