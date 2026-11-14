#include "../include/ceiling3_sdk.h"

static ceiling3_guard_state_t g_guard_state;

static const ceiling3_guard_cfg_t g_default_guard_cfg = {
    .disconnect_reboot_ms = 300000u,  /* 5 minutes offline -> warm reboot */
    .local_stale_reboot_ms = 900000u, /* 15 minutes LOCAL without healthy link -> warm reboot */
    .reboot_even_when_on = 1u,
};

static int has_elapsed_ms(uint32_t now_ms, uint32_t since_ms, uint32_t threshold_ms)
{
    if (threshold_ms == 0u) {
        return 0;
    }
    return ((uint32_t)(now_ms - since_ms)) > threshold_ms;
}

static int trigger_warm_reboot(ceiling3_guard_state_t *st, const char *reason)
{
    if (st->reboot_triggered) {
        return 0;
    }
    st->reboot_triggered = 1u;
    st->reboot_count++;
    if (g_log_level < 4) {
        arch_dbg_log_begin("\x1b[0;31m");
        log_printf("[E] %s: triggering warm reboot (%s,%d)\r\n",
                   reason, "ceiling3_guard", __LINE__);
        arch_dbg_log_end("\x1b[0m");
    }
    miio_reboot((uint32_t)(uintptr_t)g_miio_net.miio_handle, reason);
    return 1;
}

void ceiling3_guard_init(ceiling3_guard_state_t *st)
{
    st->local_since_ms = 0u;
    st->local_tracking = 0u;
    st->reboot_triggered = 0u;
    st->reboot_count = 0u;
}

/*
 * State machine step callable both from RTOS 500 ms timer and host tests.
 * Returns 1 if a warm reboot was triggered on this tick, 0 otherwise.
 */
int ceiling3_network_monitor_step(
    ceiling3_guard_state_t *st,
    const ceiling3_guard_cfg_t *cfg,
    bool local_link_healthy)
{
    const int net_state = miio_net_get_state();
    const uint32_t now_ms = arch_os_ms_now();

    if (net_state == MIIO_NET_CLOUD) {
        st->local_tracking = 0u;
        st->reboot_triggered = 0u;
        g_offline_since_ms = (uint64_t)now_ms;
        return 0;
    }

    if (net_state == MIIO_NET_LOCAL) {
        g_offline_since_ms = (uint64_t)now_ms;
        if (local_link_healthy || cfg->local_stale_reboot_ms == 0u) {
            st->local_tracking = 0u;
            return 0;
        }
        if (!st->local_tracking) {
            st->local_since_ms = now_ms;
            st->local_tracking = 1u;
            return 0;
        }
        if (has_elapsed_ms(now_ms, st->local_since_ms, cfg->local_stale_reboot_ms)) {
            return trigger_warm_reboot(st, "cmd.reboot");
        }
        return 0;
    }

    st->local_tracking = 0u;

    if (net_state == MIIO_NET_DISCONNECTED) {
        const uint64_t since = g_offline_since_ms;
        if (since == 0u) {
            return 0;
        }
        if (!cfg->reboot_even_when_on && g_yglc_settings_ptr != NULL &&
            g_yglc_settings_ptr->power != YGLC_POWER_OFF) {
            return 0;
        }
        if (has_elapsed_ms(now_ms, (uint32_t)since, cfg->disconnect_reboot_ms)) {
            return trigger_warm_reboot(st, "cmd.reboot");
        }
        return 0;
    }

    /* UNPROV / UAP / UPDATING: keep timestamp fresh so firmware upgrades are never interrupted */
    st->reboot_triggered = 0u;
    g_offline_since_ms = (uint64_t)now_ms;
    return 0;
}

__attribute__((used))
void ceiling3_network_monitor_hook(void)
{
    /* Conservative default until N9/ARP probe callback is wired */
    (void)ceiling3_network_monitor_step(&g_guard_state, &g_default_guard_cfg, true);
}

/*
 * Compact zero-BSS in-place replacement for yglc_network_monitor (@ 0x100bd918,
 * 224-byte budget up to 0x100bd9f8):
 *  - DISCONNECTED (2): reboots after > 300 000 ms regardless of lamp power.
 *  - LOCAL (3): probes default gateway every 60 s via etharp_request under
 *    lock_tcpip_core and reboots if gateway ARP stays missing/stale (>= 90 s age)
 *    for > 300 000 ms (handles N9 hangs & silent AP drops with live IPv4).
 *  - Other states (UNPROV/UAP/CLOUD/UPDATING): resets both tracking words.
 */
#ifndef HOST_TEST
__attribute__((section(".text.ceiling3_inplace_monitor"), used))
#endif
void ceiling3_yglc_network_monitor_inplace(void)
{
    volatile uint32_t *st = CEILING3_MONITOR_STATE_PTR; /* [0]=dead_since, [1]=last_probe */
    uint8_t net = FUN_100afc14();
    uint32_t now = arch_os_ms_now() | 1u;

    if (net == MIIO_NET_LOCAL) {
        uint8_t *nif = FUN_10083558(2); /* st2 */
        if (nif != NULL && *(uint32_t *)(nif + 0x0cu) != 0u) {
            uint32_t gw = *(uint32_t *)(nif + 0x0cu);
            FUN_10083808(CEILING3_TCPIP_LOCK_PTR);
            uint32_t now_s = now / 1000u;
            uint32_t alive = 0u;
            ceiling3_arp_entry_t *e = CEILING3_ARP_TABLE_PTR;
            ceiling3_arp_entry_t *end = e + 10;
            do {
                if (e->state >= 2u && e->ipaddr == gw && (uint32_t)(now_s - e->ts) < 90u) {
                    alive = 1u;
                }
            } while (++e < end);
            if (st[1] == 0u || (uint32_t)(now - st[1]) >= 60000u) {
                st[1] = now;
                FUN_1007d658(nif, nif + 0x0cu);
            }
            FUN_1008381c(CEILING3_TCPIP_LOCK_PTR);
            if (!alive) {
                goto track_dead;
            }
        }
        st[0] = 0u;
        return;
    }

    if (net == MIIO_NET_DISCONNECTED) {
track_dead:
        if (st[0] == 0u) {
            st[0] = now;
        } else if ((uint32_t)(now - st[0]) > 300000u) {
            miio_reboot(CEILING3_MIIO_HANDLE, CEILING3_CMD_REBOOT_STR);
        }
        return;
    }

    st[0] = 0u;
    st[1] = 0u;
}

/* Equivalent in C of patch #2 (0x100af298): force Wi-Fi/lwIP restart regardless of AP count */
__attribute__((used))
int ceiling3_should_force_wifi_restart(int ap_count)
{
    (void)ap_count;
    return 1;
}
