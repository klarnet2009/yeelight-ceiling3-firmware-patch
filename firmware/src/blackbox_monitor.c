/*
 * Network monitor v3 + "black box" event log for yeelink.light.ceiling3 2.0.2_0048.
 * Lives in the EXT region appended after the stock segment-0 image (0x10150650,
 * firmware/ld/ceiling3_ext_bb.ld); the bootloader writes a grown segment 0 (verified on the
 * lamp 14.11.2026).
 *
 * Entry points (patch_ota.py --hybrid):
 *   bb_monitor_tick  <- B.W at 0x100bd918 (yglc_network_monitor; yglc software timer, every
 *                       500 ms, runs in yglc_task - the task that also drives the light)
 *   bb_on_crash      <- BL at 0x100ca1da, the end of the register-dump function 0x100c9f38
 *                       that EVERY fault handler calls (HardFault, MemManage, BusFault,
 *                       UsageFault, DebugMon/stack overflow; asserts end in a forced fault)
 *
 * ---- Monitor ---------------------------------------------------------------------------
 *   CLOUD / LOCAL       : the lwIP core lock is try-taken with a ZERO timeout. Busy on every
 *                         tick for 5 min = lwIP stuck (reason L). (v2 waited forever on that
 *                         lock in LOCAL and could freeze yglc_task, i.e. light control; in
 *                         CLOUD nothing was checked at all.)
 *   LOCAL               : + gateway ARP probe every 60 s; no fresh ARP entry (reason A).
 *                         No gateway configured -> nothing to probe, healthy.
 *   DISCONNECTED        : dead (reason D)
 *   UNPROV/UAP/UPDATING : never reboots (provisioning, OTA)
 *   dead longer than the limit -> token R, log flushed, miio "cmd.reboot". That reboot is
 *   asynchronous (4 s timer, then hooks that stop Wi-Fi, then hal_sys_reboot); nothing more
 *   is logged meanwhile, and if the lamp is still running 30 s later hal_sys_reboot() is
 *   called directly (the hooks can block on a stuck Wi-Fi core).
 *   Limit: 5 min; 30 min if the previous boot was ended by the monitor; 6 h from the third
 *   reboot in a row (router off / password changed: no 5-minute blink loop). The series
 *   survives in RAM and, if RAM is lost, in the digit of the last R token of the flash log;
 *   it is cleared when CLOUD is reached or the lamp is power-cycled. Wi-Fi itself keeps
 *   reconnecting all the time without reboots.
 *
 * ---- Black box ---------------------------------------------------------------------------
 * Text, newest last, at most BB_TEXT chars, one property: get_prop ["bblog"]. Ask for it
 * alone: the stock get_prop has a 512-byte reply buffer and sends garbage past its end when
 * the total is longer (snprintf with an exhausted size; no out-of-bounds write).
 *   B<t>[,u<m>][,pc<x>,lr<x>,cf<x>][,t<x>];
 *        boot. t: previous reset 0 = power-on/unknown, 1 = software reboot, 2 = crash.
 *        u = minutes the previous run lasted, t = UTC (hex s) when it was last alive,
 *        pc/lr/cf = fault PC, LR, SCB->CFSR. u/t/pc need the RAM block to survive the reset.
 *   T<x>;                 UTC (hex s) of this boot, logged once when the clock is known
 *   N<s>,<m>;             network state s (0 unprov,1 uap,2 disconnected,3 local,4 cloud,
 *                         5 updating) at uptime m minutes
 *   R<r><n>,<m>[,t<x>];   monitor reboot, r = D/A/L, n = number in the series
 * When full, the oldest routine token (N, T, B0, B1) is dropped first; incidents (R, B2) are
 * kept as long as they take no more than BB_INCIDENT_MAX chars.
 * RAM: noinit block at BB_ADDR (bb_shared.h); validated by magic + length + printable text,
 * rebuilt from flash otherwise.
 * Flash: psm "app_data"/"bblog": 60 s after boot, before a monitor reboot, and at most every
 * 10 min while something changed, never during an OTA (psm writes from yglc_task are stock
 * behaviour: set_ps; a write pauses light transitions for its duration).
 * get_prop reads the text from another task while yglc_task edits it: text[BB_TEXT] is
 * always 0, so a reader always finds a terminator (worst case one garbled reply).
 *
 * ---- Crash path --------------------------------------------------------------------------
 * Stock: every fault handler dumps registers, then ~320 KB of RAM as hex text to the UART
 * (minutes), then arch_crash_reboot(). patch_ota.py (0x100ca2c4) makes the register-dump
 * function reboot right away; bb_on_crash() stores PC/LR/CFSR in the RAM block first and,
 * should arch_crash_reboot() return without resetting, uses the reset sequence of
 * hal_sys_reboot (WDT software reset) that every normal reboot of this lamp uses.
 *
 * ---- Fail-safe ---------------------------------------------------------------------------
 * A boot "follows a crash" if g_warm_boot_type == 2 or the RAM crash latch is set. On such a
 * boot this code is passive if the RAM block did not survive, if the crash was inside this
 * code, if the crashed run lasted under a minute, or if it is the second such boot in a row
 * without 10 healthy minutes in between. Passive = no log, no property, no lwIP checks; only
 * the stock-equivalent rule "no network for 30 min -> reboot" stays. The crash record is
 * kept and logged by the next active boot. So this code cannot keep the lamp in a reboot
 * loop; a power cycle or a normal reboot re-arms it.
 */
#include <stddef.h>
#include <stdint.h>

#include "../include/bb_shared.h"

/* ---- firmware symbols (ceiling3_symbols.ld) ---- */
uint8_t  FUN_100afc14(void);                                   /* miio_net_get_state        */
uint8_t *FUN_10083558(int idx);                                /* netif by index (2 = st2)  */
int      FUN_1007d658(void *netif, const void *ipaddr);        /* etharp_request            */
uint32_t FUN_100a8650(void);                                   /* UTC seconds (0-based until NTP) */
uint32_t arch_os_ms_now(void);                                 /* FreeRTOS tick, ms         */
int      xQueueSemaphoreTake(void *q, uint32_t ticks);
int      xQueueGenericSend(void *q, const void *item, uint32_t ticks, int pos);
int      miio_reboot(uint32_t handle, const char *reason);     /* async: resets ~4 s later  */
uint32_t hal_sys_reboot(uint32_t magic, uint32_t cmd);         /* saves type 1, WDT reset   */
void     arch_crash_reboot(void);
void     hal_wdt_init(int mode, int a, int b, int reset);
void     hal_wdt_software_reset(void);
int      psm_get_variable(const char *grp, const char *key, char *buf, uint32_t len);
int      psm_set_variable(const char *grp, const char *key, const char *val);
void     yglc_setting_entry_new(void *ptr, int size, const char *name, char nv, char crc,
                                char report, uint8_t is_str, const char *alias,
                                void *get, void *set, void *rep);
void     FUN_100c2ae0(void);                                   /* string entry: psm get     */
void     FUN_100c2af0(void);                                   /* string entry: psm set     */
void     FUN_100c2b34(void);                                   /* string entry: strncpy out */
void    *memmove(void *d, const void *s, size_t n);
void    *memset(void *d, int c, size_t n);
extern const char __ext_start[], __ext_end[];                  /* ceiling3_ext_bb.ld        */

enum { NET_DISCONNECTED = 2, NET_LOCAL = 3, NET_CLOUD = 4, NET_UPDATING = 5 };

#define TCPIP_LOCK        (*(void *const volatile *)0x20002c40u)   /* lock_tcpip_core       */
#define ARP_TABLE         ((const arp_entry_t *)0x2000264cu)
#define ARP_TABLE_SIZE    10
#define MIIO_HANDLE       (*(volatile uint32_t *)0x20004b24u)
#define CMD_REBOOT_STR    ((const char *)0x101241b1u)          /* "cmd.reboot"              */
#define HAL_REBOOT_MAGIC  0x20160626u                          /* hal_sys_reboot key        */
#define FAULT_REGS        ((const volatile uint32_t *)0x20039a50u)   /* +0x38 lr, +0x3c pc   */
#define SCB_CFSR          (*(const volatile uint32_t *)0xE000ED28u)

#define DEAD_LIMIT_MS     300000u      /* first reboot of a series                          */
#define DEAD_LIMIT2_MS    1800000u     /* second; also the passive-mode limit               */
#define DEAD_LIMIT3_MS    21600000u    /* third and later                                   */
#define REBOOT_GRACE_MS   30000u       /* miio_reboot must have reset the chip by then      */
#define PROBE_MS          60000u
#define ARP_FRESH_S       90u
#define FLUSH_MS          600000u
#define BOOT_FLUSH_MS     60000u
#define UTC_VALID         0x60000000u  /* 2021-01-14: anything below is "clock not set"      */
#define EARLY_CRASH_S     60u
#define HEALTHY_S         600u

typedef struct {
    void    *q;
    uint32_t ipaddr;
    void    *netif;
    uint8_t  ethaddr[6];
    uint16_t ctime;
    uint8_t  state;          /* >= 2: stable */
    uint8_t  pad[3];
    uint32_t ts;             /* tick / 1000 of the last update (MTK lwIP extension) */
} arp_entry_t;
_Static_assert(sizeof(arp_entry_t) == 0x1c, "arp entry");

#define BB_TEXT         240u
#define BB_INCIDENT_MAX 150u  /* chars of BB_TEXT that incident tokens may hold on to        */
#define BB_TOK          72u   /* longest token: B2,u<8>,pc<8>,lr<8>,cf<8>,t<8>; = 56        */

typedef struct {
    uint32_t magic, magic_inv;
    uint32_t crash_latch;        /* BB_CRASHED (fault context) / BB_CRASH_SEEN (bb_boot) / 0 */
    uint32_t crash_pc, crash_lr, crash_cfsr;                /* written in fault context     */
    uint32_t last_alive_s;       /* uptime of this run, every tick                          */
    uint32_t last_alive_utc;     /* UTC of this run, every tick (0 = clock not set)         */
    uint32_t crash_boots;        /* consecutive boots that followed a crash                 */
    uint32_t last_flush_ms;
    uint32_t mon_reboots;        /* monitor reboots in a row without CLOUD in between       */
    uint32_t reboot_req_ms;      /* when the monitor asked for a reboot                     */
    uint8_t  prev_net, dirty, passive, t_logged;
    uint8_t  rebooting;          /* reboot requested: stop logging, wait for the reset      */
    uint8_t  crash_unlogged;     /* crash_pc/lr/cfsr not yet written to the log             */
    uint8_t  pad[2];
    uint32_t len;
    char     text[BB_TEXT + 4];  /* text[BB_TEXT] stays 0                                   */
} bb_t;
_Static_assert(offsetof(bb_t, crash_latch) == BB_CRASH_OFF, "crash latch offset (bb_shared.h)");
_Static_assert(offsetof(bb_t, text) == 0x3c, "bb text offset (tools/emu_bb_test.py)");
_Static_assert(sizeof(bb_t) <= 0x200, "bb block");
#define BB ((bb_t *)BB_ADDR)

static const char k_grp[] = "app_data";
static const char k_key[] = "bblog";

#define BB_FN __attribute__((noinline))

/* ---- tiny formatter (no printf: bounded, no libc stack use) ---- */

static char *put_s(char *p, const char *s)
{
    while (*s != '\0') {
        *p++ = *s++;
    }
    return p;
}

BB_FN static char *put_num(char *p, uint32_t v, uint32_t base)
{
    char tmp[10];
    int i = 0;
    do {
        uint32_t d = v % base;
        tmp[i++] = (char)(d < 10u ? '0' + d : 'a' + d - 10u);
        v /= base;
    } while (v != 0u);
    while (i > 0) {
        *p++ = tmp[--i];
    }
    return p;
}

/* ",<key><hex>" */
BB_FN static char *put_kx(char *p, const char *key, uint32_t v)
{
    *p++ = ',';
    return put_num(put_s(p, key), v, 16u);
}

/* ---- log text ---- */

/* length of a well-formed text (printable ASCII, 0-terminated within BB_TEXT), else -1 */
BB_FN static int text_len(const char *t)
{
    uint32_t n = 0;
    while (n < BB_TEXT && t[n] >= 0x20 && t[n] < 0x7f) {
        n++;
    }
    return (t[n] == '\0') ? (int)n : -1;
}

/* R, B2, and any boot record that carries a crash PC ('p' occurs only in ",pc") */
static int tok_is_incident(const char *t)
{
    if (t[0] == 'R') {
        return 1;
    }
    if (t[0] != 'B') {
        return 0;
    }
    if (t[1] == '2') {
        return 1;
    }
    for (uint32_t i = 2; i < BB_TOK && t[i] >= 0x20 && t[i] != ';'; i++) {
        if (t[i] == 'p') {
            return 1;
        }
    }
    return 0;
}

/* index just past the token that starts at i */
static uint32_t tok_end(const bb_t *b, uint32_t i)
{
    while (i < b->len && b->text[i] != ';') {
        i++;
    }
    return (i < b->len) ? i + 1u : b->len;
}

/*
 * Make room: drop the oldest routine token; incidents go only when nothing else is left or
 * when they occupy more than BB_INCIDENT_MAX chars (so old incidents cannot crowd out the
 * recent routine history, which is all there is to see after a hang).
 */
BB_FN static void bb_drop_one(bb_t *b)
{
    uint32_t i = 0, first_low = b->len, first_inc = b->len, inc_bytes = 0;
    while (i < b->len) {
        uint32_t e = tok_end(b, i);
        if (tok_is_incident(b->text + i)) {
            inc_bytes += e - i;
            if (first_inc == b->len) {
                first_inc = i;
            }
        } else if (first_low == b->len) {
            first_low = i;
        }
        i = e;
    }
    uint32_t victim = (first_low == b->len || inc_bytes > BB_INCIDENT_MAX) ? first_inc : first_low;
    if (victim >= b->len) {
        victim = 0u;
    }
    uint32_t e = tok_end(b, victim);
    memmove(b->text + victim, b->text + e, b->len - e);
    b->len -= e - victim;
    b->text[b->len] = 0;
}

/* append the token [tok, end) */
BB_FN static void bb_add(const char *tok, const char *end)
{
    bb_t *b = BB;
    uint32_t n = (uint32_t)(end - tok);
    if (b->len > BB_TEXT) {                     /* cannot happen (validated at boot) */
        b->len = 0u;
    }
    while (b->len != 0u && b->len + n > BB_TEXT) {
        bb_drop_one(b);
    }
    if (n > BB_TEXT) {
        return;
    }
    memmove(b->text + b->len, tok, n);
    b->len += n;
    b->text[b->len] = 0;
    b->dirty = 1u;
}

BB_FN static void bb_flush(uint32_t now)
{
    bb_t *b = BB;
    b->last_flush_ms = now;
    b->dirty = 0u;
    (void)psm_set_variable(k_grp, k_key, b->text);
}

static uint32_t utc_now(void)
{
    uint32_t t = FUN_100a8650();
    return (t >= UTC_VALID) ? t : 0u;
}

/* "<kind>[<sub>]<arg>,<uptime min>[,t<utc>];" */
BB_FN static void bb_event(char kind, char sub, uint32_t arg, uint32_t now, uint32_t utc)
{
    char tok[BB_TOK];
    char *p = tok;
    *p++ = kind;
    if (sub != 0) {
        *p++ = sub;
    }
    p = put_num(p, arg, 10u);
    *p++ = ',';
    p = put_num(p, now / 60000u, 10u);
    if (utc != 0u) {
        p = put_kx(p, "t", utc);
    }
    *p++ = ';';
    bb_add(tok, p);
}

/*
 * Series number of the monitor reboot that ended the previous boot, from the text: the last
 * token, ignoring N/T tokens after it, must be "R<reason><n>,...".
 */
BB_FN static uint32_t reboot_series_from_text(const bb_t *b)
{
    uint32_t i = b->len;
    while (i > 0u) {
        uint32_t s = i - 1u;                    /* text[s] is the token's ';' */
        while (s > 0u && b->text[s - 1u] != ';') {
            s--;
        }
        const char *t = b->text + s;
        if (t[0] == 'R') {
            return (t[2] >= '1' && t[2] <= '9') ? (uint32_t)(t[2] - '0') : 0u;
        }
        if (t[0] != 'N' && t[0] != 'T') {
            return 0u;
        }
        i = s;
    }
    return 0u;
}

static int in_ext(uint32_t a)
{
    return a >= (uint32_t)(uintptr_t)__ext_start && a < (uint32_t)(uintptr_t)__ext_end;
}

/* first tick of a boot */
BB_FN static void bb_boot(uint32_t now)
{
    bb_t *b = BB;
    uint32_t type = BB_BOOT_TYPE;
    int valid = (b->magic == BB_MAGIC && b->magic_inv == ~BB_MAGIC &&
                 b->len <= BB_TEXT && text_len(b->text) == (int)b->len);
    int crashed = (valid && b->crash_latch == BB_CRASHED);
    int after_crash = (type == 2u || crashed);

    if (!valid) {
        memset(b, 0, sizeof *b);
        b->magic = BB_MAGIC;
        b->magic_inv = ~BB_MAGIC;
    }
    /* state for the fail-safe is stored before anything that could fault */
    b->crash_latch = after_crash ? BB_CRASH_SEEN : 0u;
    b->crash_boots = after_crash ? b->crash_boots + 1u : 0u;
    if (crashed) {
        b->crash_unlogged = 1u;
    }
    int series_in_ram = (valid && b->rebooting);
    b->rebooting = 0u;
    b->passive = (uint8_t)(after_crash &&
                           (!valid || b->crash_boots >= 2u || b->last_alive_s < EARLY_CRASH_S ||
                            (crashed && (in_ext(b->crash_pc) || in_ext(b->crash_lr)))));
    if (b->passive) {
        if (!valid) {
            b->len = BB_TEXT + 1u;              /* keep the block invalid: the next boot */
        }                                       /* reloads the log from flash            */
        return;
    }
    if (!valid) {                               /* RAM block lost: reload the flash copy */
        int n = -1;
        if (psm_get_variable(k_grp, k_key, b->text, BB_TEXT + 1u) >= 0) {
            b->text[BB_TEXT] = 0;
            n = text_len(b->text);
        }
        if (n < 0) {
            memset(b->text, 0, sizeof b->text);
            n = 0;
        }
        b->len = (uint32_t)n;
    }
    if (!series_in_ram) {
        b->mon_reboots = (type == 1u) ? reboot_series_from_text(b) : 0u;
    }

    char tok[BB_TOK];
    char *p = tok;
    *p++ = 'B';
    p = put_num(p, type, 10u);
    if (valid) {
        *p++ = ',';
        *p++ = 'u';
        p = put_num(p, b->last_alive_s / 60u, 10u);
        if (b->crash_unlogged) {
            p = put_kx(p, "pc", b->crash_pc);
            p = put_kx(p, "lr", b->crash_lr);
            p = put_kx(p, "cf", b->crash_cfsr);
        }
        if (b->last_alive_utc != 0u) {
            p = put_kx(p, "t", b->last_alive_utc);
        }
    }
    *p++ = ';';
    b->crash_unlogged = 0u;
    b->last_alive_s = 0u;
    b->last_alive_utc = 0u;
    b->prev_net = 0xffu;
    b->t_logged = 0u;
    bb_add(tok, p);
    b->last_flush_ms = now + BOOT_FLUSH_MS - FLUSH_MS;   /* first flush 60 s after boot */

    yglc_setting_entry_new(b->text, (int)(BB_TEXT + 1u), k_key, 0, 0, 0, 1u, k_key,
                           (void *)FUN_100c2ae0, (void *)FUN_100c2af0, (void *)FUN_100c2b34);
}

/* ---- monitor ---- */

/* 1 healthy, 0 gateway silent, -1 lwIP core lock busy */
BB_FN static int lwip_check(uint32_t now, int probe_gateway)
{
    volatile uint32_t *st = BB_MON_ST;
    void *lock = TCPIP_LOCK;
    if (lock == NULL) {
        return 1;
    }
    if (xQueueSemaphoreTake(lock, 0u) != 1) {
        return -1;                              /* never wait: yglc_task must not block */
    }
    int alive = 1;
    uint8_t *nif = probe_gateway ? FUN_10083558(2) : NULL;
    uint32_t gw = (nif != NULL) ? *(uint32_t *)(nif + 0x0cu) : 0u;
    if (gw != 0u) {
        uint32_t now_s = now / 1000u;
        alive = 0;
        for (const arp_entry_t *e = ARP_TABLE; e < ARP_TABLE + ARP_TABLE_SIZE; e++) {
            if (e->state >= 2u && e->ipaddr == gw && (uint32_t)(now_s - e->ts) < ARP_FRESH_S) {
                alive = 1;
            }
        }
        if (st[1] == 1u || (uint32_t)(now - st[1]) >= PROBE_MS) {
            st[1] = now;                        /* now is odd and > 1 */
            (void)FUN_1007d658(nif, nif + 0x0cu);
        }
    }
    (void)xQueueGenericSend(lock, NULL, 0u, 0);
    return alive;
}

BB_FN static void mon_reboot(char reason, uint32_t now)
{
    bb_t *b = BB;
    b->mon_reboots++;
    bb_event('R', reason, b->mon_reboots > 9u ? 9u : b->mon_reboots, now, utc_now());
    bb_flush(now);
    b->rebooting = 1u;
    b->reboot_req_ms = now;
    (void)miio_reboot(MIIO_HANDLE, CMD_REBOOT_STR);
}

/* passive mode: only the stock-equivalent rule, no log, no lwIP access */
BB_FN static void passive_tick(uint32_t now)
{
    volatile uint32_t *st = BB_MON_ST;
    if (FUN_100afc14() != NET_DISCONNECTED) {
        st[0] = 0u;
    } else if (st[0] == 0u) {
        st[0] = now;
    } else if ((uint32_t)(now - st[0]) > DEAD_LIMIT2_MS) {
        st[0] = 0u;
        (void)miio_reboot(MIIO_HANDLE, CMD_REBOOT_STR);
    }
}

void bb_monitor_tick(void)
{
    volatile uint32_t *st = BB_MON_ST;
    bb_t *b = BB;
    uint32_t now = arch_os_ms_now() | 1u;       /* odd: never equals the 0 / 1 markers */

    if (st[1] == 0u) {                          /* .bss: 0 only on the first tick of a boot */
        st[1] = 1u;
        bb_boot(now);
    }
    if (b->passive) {
        passive_tick(now);
        return;
    }
    if (b->rebooting) {                         /* reboot requested: nothing else may touch */
        if ((uint32_t)(now - b->reboot_req_ms) > REBOOT_GRACE_MS) {   /* the log any more  */
            (void)hal_sys_reboot(HAL_REBOOT_MAGIC, 0u);
        }
        return;
    }
    uint32_t utc = utc_now();
    b->last_alive_s = now / 1000u;
    b->last_alive_utc = utc;
    if (b->last_alive_s >= HEALTHY_S) {
        b->crash_boots = 0u;                    /* crashes far apart are not a crash loop */
    }

    uint32_t net = FUN_100afc14();
    if (net != b->prev_net) {
        b->prev_net = (uint8_t)net;
        bb_event('N', 0, net, now, 0u);
    }
    if (utc != 0u && !b->t_logged) {
        char tok[BB_TOK];
        char *p = tok;
        *p++ = 'T';
        p = put_num(p, utc - now / 1000u, 16u);
        *p++ = ';';
        b->t_logged = 1u;
        bb_add(tok, p);
    }
    if (b->dirty && net != NET_UPDATING && (uint32_t)(now - b->last_flush_ms) >= FLUSH_MS) {
        bb_flush(now);
    }

    char reason = 0;
    if (net == NET_CLOUD || net == NET_LOCAL) {
        int r = lwip_check(now, net == NET_LOCAL);
        if (r < 0) {
            reason = 'L';
        } else if (r == 0) {
            reason = 'A';
        } else if (net == NET_CLOUD) {
            b->mon_reboots = 0u;
        }
    } else if (net == NET_DISCONNECTED) {
        reason = 'D';
    }                                           /* UNPROV / UAP / UPDATING: never reboot */
    if (reason == 0) {
        st[0] = 0u;
        return;
    }
    if (st[0] == 0u) {
        st[0] = now;
        return;
    }
    uint32_t limit = b->mon_reboots == 0u ? DEAD_LIMIT_MS
                   : b->mon_reboots == 1u ? DEAD_LIMIT2_MS : DEAD_LIMIT3_MS;
    if ((uint32_t)(now - st[0]) > limit) {
        st[0] = 0u;
        mon_reboot(reason, now);
    }
}

/* Fault context (any fault handler -> register dump 0x100c9f38 -> here): RAM writes only. */
void bb_on_crash(void)
{
    bb_t *b = BB;
    if (b->magic == BB_MAGIC && b->magic_inv == ~BB_MAGIC) {
        b->crash_pc = FAULT_REGS[0x3c / 4];
        b->crash_lr = FAULT_REGS[0x38 / 4];
        b->crash_cfsr = SCB_CFSR;
        b->crash_latch = BB_CRASHED;
    }
    arch_crash_reboot();                        /* stock: saves reset type 2, waits 3 s, resets */
    hal_wdt_init(0, 0, 0, 1);                   /* still alive: the reset every normal reboot */
    hal_wdt_software_reset();                   /* of this lamp uses (hal_sys_reboot)         */
}
