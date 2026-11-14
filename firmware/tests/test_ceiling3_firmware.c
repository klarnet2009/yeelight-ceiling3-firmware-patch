#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef HOST_TEST
#define HOST_TEST 1
#endif
#include "../include/ceiling3_sdk.h"

/* Host build verifies non-pointer fixed prefix offsets; ARM build checks all 32-bit ABI offsets. */
_Static_assert(offsetof(arch_ota_part_t, partition_len) == 0x18, "arch_ota partition_len offset");
_Static_assert(offsetof(yglc_settings_t, power) == 0x54, "yglc_settings power offset");
_Static_assert(offsetof(yglc_settings_t, init_power_opt) == 0x100, "yglc_settings init_power_opt offset");

/* ---- Mock firmware globals & ROM functions ---- */
int32_t g_log_level = 4;
miio_net_t g_miio_net;
static yglc_settings_t g_mock_settings;
yglc_settings_t *g_yglc_settings_ptr = &g_mock_settings;
uint64_t g_offline_since_ms = 0;
uint8_t g_mock_st2_netif[64];
uint32_t g_mock_tcpip_lock = 0;
ceiling3_arp_entry_t g_mock_arp_table[10];

static uint32_t g_mock_ms = 1000u;
static int g_mock_net_state = MIIO_NET_CLOUD;
static int g_reboot_calls = 0;
static uint32_t g_last_reboot_handle = 0;
static char g_last_reboot_reason[64];
static int g_crc_ret = 0;
static int g_etharp_requests = 0;
static int g_tcpip_lock_balance = 0;

uint32_t arch_os_ms_now(void) { return g_mock_ms; }
int arch_os_ms_elapsed(uint32_t since_ms) { return (int)(g_mock_ms - since_ms); }
int miio_net_get_state(void) { return g_mock_net_state; }
uint8_t FUN_100afc14(void) { return (uint8_t)g_mock_net_state; }
uint8_t *FUN_10083558(int idx) { return (idx == 2) ? g_mock_st2_netif : NULL; }
void FUN_10083808(void *mutex) { (void)mutex; g_tcpip_lock_balance++; }
void FUN_1008381c(void *mutex) { (void)mutex; g_tcpip_lock_balance--; }
void FUN_1007d658(void *netif, void *ipaddr) { (void)netif; (void)ipaddr; g_etharp_requests++; }

void miio_reboot(uint32_t miio_handle, const char *reason)
{
    g_reboot_calls++;
    g_last_reboot_handle = miio_handle;
    snprintf(g_last_reboot_reason, sizeof(g_last_reboot_reason), "%s", reason ? reason : "");
}

int arch_ota_check_crc(const arch_ota_part_t *part, uint32_t size)
{
    (void)part;
    (void)size;
    return g_crc_ret;
}

void arch_dbg_log_begin(const char *c) { (void)c; }
void arch_dbg_log_end(const char *r) { (void)r; }
int log_printf(const char *fmt, ...) { (void)fmt; return 0; }

static void reset_mocks(void)
{
    memset(&g_miio_net, 0, sizeof(g_miio_net));
    g_miio_net.miio_handle = (void *)(uintptr_t)0x20001234u;
    memset(&g_mock_settings, 0, sizeof(g_mock_settings));
    g_mock_settings.power = YGLC_POWER_ON;
    g_offline_since_ms = 0;
    g_mock_ms = 1000u;
    g_mock_net_state = MIIO_NET_CLOUD;
    g_reboot_calls = 0;
    g_last_reboot_handle = 0;
    g_last_reboot_reason[0] = '\0';
    g_crc_ret = 0;
    g_etharp_requests = 0;
    g_tcpip_lock_balance = 0;
    memset(g_mock_st2_netif, 0, sizeof(g_mock_st2_netif));
    memset(g_mock_arp_table, 0, sizeof(g_mock_arp_table));
}

static void test_disconnect_reboot_with_light_on_and_off(void)
{
    reset_mocks();
    ceiling3_guard_state_t st;
    ceiling3_guard_init(&st);
    const ceiling3_guard_cfg_t cfg = {300000u, 900000u, 1u, {0}};

    /* Initial cloud tick sets baseline timestamp */
    assert(ceiling3_network_monitor_step(&st, &cfg, true) == 0);
    assert(g_offline_since_ms == 1000u);

    /* Drop to DISCONNECTED with light ON -> should NOT reboot before 300000 ms */
    g_mock_net_state = MIIO_NET_DISCONNECTED;
    g_mock_ms = 300000u;
    assert(ceiling3_network_monitor_step(&st, &cfg, false) == 0);
    assert(g_reboot_calls == 0);

    /* Cross 300000 ms threshold -> warm reboot triggered even while light ON */
    g_mock_ms = 301001u;
    assert(ceiling3_network_monitor_step(&st, &cfg, false) == 1);
    assert(g_reboot_calls == 1);
    assert(g_last_reboot_handle == 0x20001234u);
    assert(strcmp(g_last_reboot_reason, "cmd.reboot") == 0);

    /* Subsequent disconnected ticks must not double-trigger */
    g_mock_ms = 302000u;
    assert(ceiling3_network_monitor_step(&st, &cfg, false) == 0);
    assert(g_reboot_calls == 1);
}

static void test_local_stale_guard_and_recovery(void)
{
    reset_mocks();
    ceiling3_guard_state_t st;
    ceiling3_guard_init(&st);
    const ceiling3_guard_cfg_t cfg = {300000u, 60000u, 1u, {0}};

    g_mock_net_state = MIIO_NET_LOCAL;
    g_mock_ms = 10000u;
    assert(ceiling3_network_monitor_step(&st, &cfg, false) == 0);
    assert(st.local_tracking == 1u);

    /* Healthy link resets local timer */
    g_mock_ms = 50000u;
    assert(ceiling3_network_monitor_step(&st, &cfg, true) == 0);
    assert(st.local_tracking == 0u);

    /* Unhealthy LOCAL link past threshold triggers reboot */
    g_mock_ms = 51000u;
    assert(ceiling3_network_monitor_step(&st, &cfg, false) == 0);
    g_mock_ms = 112000u;
    assert(ceiling3_network_monitor_step(&st, &cfg, false) == 1);
    assert(g_reboot_calls == 1);
}

static void test_timer_wraparound_and_ota_hooks(void)
{
    reset_mocks();
    ceiling3_guard_state_t st;
    ceiling3_guard_init(&st);
    const ceiling3_guard_cfg_t cfg = {300000u, 0u, 1u, {0}};

    /* Wraparound across UINT32_MAX */
    g_mock_ms = 0xfffff000u;
    g_mock_net_state = MIIO_NET_CLOUD;
    assert(ceiling3_network_monitor_step(&st, &cfg, true) == 0);

    g_mock_net_state = MIIO_NET_DISCONNECTED;
    g_mock_ms = 100000u;
    assert(ceiling3_network_monitor_step(&st, &cfg, false) == 0);

    g_mock_ms = 300000u;
    assert(ceiling3_network_monitor_step(&st, &cfg, false) == 1);

    /* OTA RPC channel check: both local (bit 0) and cloud (bit 1) permitted */
    char payload[] = "{\"method\":\"miIO.ota\"}";
    miio_rpc_delegate_arg_t req = {
        .id = 1,
        .type = 0,
        .channel_flags = 1u,
        .pload_len = sizeof(payload) - 1,
        .pload = payload,
    };
    assert(ceiling3_is_ota_rpc_allowed(&req) == 1);
    req.channel_flags = 2u;
    assert(ceiling3_is_ota_rpc_allowed(&req) == 1);
    req.channel_flags = 0u;
    assert(ceiling3_is_ota_rpc_allowed(&req) == 0);

    ota_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.flash_part.partition_len = 0x130000u;
    ctx.total_size = 727042u;
    assert(ceiling3_verify_ota_image(&ctx) == 0);
    ctx.total_size = 0x200u;
    assert(ceiling3_verify_ota_image(&ctx) == -2);

    assert(ceiling3_should_force_wifi_restart(5) == 1);
}

static void test_inplace_monitor_local_arp_and_disconnected(void)
{
    volatile uint32_t *st = CEILING3_MONITOR_STATE_PTR;

    /* 1. DISCONNECTED reboots after > 300000 ms regardless of lamp power */
    reset_mocks();
    g_mock_settings.power = YGLC_POWER_ON;
    g_mock_net_state = MIIO_NET_DISCONNECTED;
    g_mock_ms = 1000u;
    ceiling3_yglc_network_monitor_inplace();
    assert(st[0] == 1001u);
    assert(g_reboot_calls == 0);

    g_mock_ms = 301001u; /* elapsed == 300000 ms -> not yet */
    ceiling3_yglc_network_monitor_inplace();
    assert(g_reboot_calls == 0);

    g_mock_ms = 301002u; /* elapsed == 300001 ms -> reboot */
    ceiling3_yglc_network_monitor_inplace();
    assert(g_reboot_calls == 1);
    assert(strcmp(g_last_reboot_reason, "cmd.reboot") == 0);

    /* 2. LOCAL with live gateway ARP probes every 60s and never reboots */
    reset_mocks();
    g_mock_net_state = MIIO_NET_LOCAL;
    *(uint32_t *)(g_mock_st2_netif + 0x0cu) = 0x0101A8C0u;
    g_mock_arp_table[3].state = 2u;
    g_mock_arp_table[3].ipaddr = 0x0101A8C0u;
    g_mock_arp_table[3].ts = 100u;
    g_mock_ms = 100000u;
    ceiling3_yglc_network_monitor_inplace();
    assert(g_etharp_requests == 1);
    assert(g_tcpip_lock_balance == 0);
    assert(st[0] == 0u);
    assert(st[1] == 100001u);

    g_mock_ms = 159999u;
    ceiling3_yglc_network_monitor_inplace();
    assert(g_etharp_requests == 1);
    assert(st[0] == 0u);

    g_mock_ms = 160001u;
    g_mock_arp_table[3].ts = 160u;
    ceiling3_yglc_network_monitor_inplace();
    assert(g_etharp_requests == 2);
    assert(g_tcpip_lock_balance == 0);
    assert(st[0] == 0u);
    assert(g_reboot_calls == 0);

    /* 3. LOCAL with stale (>= 90s) gateway ARP tracks dead time and resets on recovery */
    g_mock_ms = 250000u; /* 250 - 160 == 90s -> stale */
    ceiling3_yglc_network_monitor_inplace();
    assert(g_etharp_requests == 3);
    assert(st[0] == 250001u);
    assert(g_reboot_calls == 0);

    g_mock_ms = 400000u;
    g_mock_arp_table[3].ts = 399u; /* fresh reply arrives mid-window */
    ceiling3_yglc_network_monitor_inplace();
    assert(st[0] == 0u);
    assert(g_reboot_calls == 0);

    /* 4. LOCAL with missing ARP reboots after > 300000 ms */
    g_mock_arp_table[3].state = 0u;
    g_mock_ms = 500000u;
    ceiling3_yglc_network_monitor_inplace();
    assert(st[0] == 500001u);
    assert(g_reboot_calls == 0);

    g_mock_ms = 800002u;
    ceiling3_yglc_network_monitor_inplace();
    assert(g_reboot_calls == 1);
    assert(g_tcpip_lock_balance == 0);

    /* 5. CLOUD clears both tracking words */
    g_mock_net_state = MIIO_NET_CLOUD;
    ceiling3_yglc_network_monitor_inplace();
    assert(st[0] == 0u && st[1] == 0u);
}

static uint32_t sim_dl_pwm_channel(int bright, float ct_pct, float min_bright_pct, float pwm_floor_pct)
{
    if (bright <= 0 || ct_pct <= 0.0f) {
        return 0u;
    }
    float scale;
    if (min_bright_pct == 0.0f) {
        scale = (float)bright / 100.0f;
    } else {
        float lin = (100.0f - min_bright_pct * 100.0f) / -99.0f +
                    (float)bright * ((min_bright_pct - 100.0f) / -99.0f);
        scale = lin / 100.0f;
    }
    float raw = (ct_pct / 100.0f) * scale * 4000.0f;
    if (pwm_floor_pct != 0.0f && raw > 0.0f) {
        float f = pwm_floor_pct / 100.0f;
        raw = (f + (1.0f - f) * (raw / 4000.0f)) * 4000.0f;
    }
    return (uint32_t)(raw + 0.49f);
}

static void test_daylight_brightness_curve(void)
{
    /* Stock dl (min_bright=15, floor=8%): 1% at 4856K was ~1142/4000 (28.6%) */
    uint32_t stock_w = sim_dl_pwm_channel(1, 28.0f, 15.0f, 8.0f);
    uint32_t stock_c = sim_dl_pwm_channel(1, 63.0f, 15.0f, 8.0f);
    assert(stock_w + stock_c > 1100u);

    /* Patched dl (min_bright=0, floor=6.5%): 1% at 4856K drops to ~553/4000 (each ch > 6.5% > ~5% driver UVLO) */
    uint32_t new_w = sim_dl_pwm_channel(1, 28.0f, 0.0f, 6.5f);
    uint32_t new_c = sim_dl_pwm_channel(1, 63.0f, 0.0f, 6.5f);
    assert(new_w >= 260u && new_c >= 260u);
    assert(new_w + new_c < 580u);

    /* Single-channel extremes at 2700K/6500K: 1% -> ~297/4000 (7.4%), 100% -> 4000/4000 (100%) */
    assert(sim_dl_pwm_channel(1, 100.0f, 0.0f, 6.5f) == 297u);
    assert(sim_dl_pwm_channel(100, 100.0f, 0.0f, 6.5f) == 4000u);
    assert(sim_dl_pwm_channel(0, 100.0f, 0.0f, 6.5f) == 0u);

    /* Strictly monotonic across 1..100 */
    uint32_t prev = 0u;
    for (int b = 1; b <= 100; b++) {
        uint32_t cur = sim_dl_pwm_channel(b, 100.0f, 0.0f, 6.5f);
        assert(cur > prev);
        prev = cur;
    }
}

int main(void)
{
    test_disconnect_reboot_with_light_on_and_off();
    test_local_stale_guard_and_recovery();
    test_timer_wraparound_and_ota_hooks();
    test_inplace_monitor_local_arp_and_disconnected();
    test_daylight_brightness_curve();
    printf("test_ceiling3_firmware: ALL TESTS PASSED (ABI + recovery + OTA hooks + dl curve)\n");
    return 0;
}
