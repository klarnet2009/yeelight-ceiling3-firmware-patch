"""Integration test of the hybrid daylight brightness in a Cortex-M4 emulator.

Runs the REAL segment 0 of a built OTA image (yglc_init_settings, light unit API, effect
stacks, trans managers, mixers, the hybrid hook) and records what reaches the PWM HAL.
Only the OS / logging / cloud reporting / HAL calls are stubbed.

Run: python tools/emu_hybrid_test.py [image.bin]   (default: the hybrid image)
"""
import os
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UcError
from unicorn import UC_HOOK_MEM_UNMAPPED
from unicorn import arm_const as A

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.argv, _args = sys.argv[:1], sys.argv[1:]
import patch_ota  # noqa: E402

IMAGE = _args[0] if _args else os.path.join(ROOT, 'yeelink.light.ceiling3-hybrid-2.0.2_0048.bin')
BASE = 0x10071000
RET = 0x10000100
HEAP = 0x20010000
SP_TOP = 0x2003f000
CH_WARM, CH_COOL, CH_NL = 0x20, 0x21, 0x1f       # hal_pwm channels 32/33 (dl), 31 (nl)
K = 10
FLOOR = 10.0

R = [A.UC_ARM_REG_R0, A.UC_ARM_REG_R1, A.UC_ARM_REG_R2, A.UC_ARM_REG_R3]


class Emu:
    def __init__(self, image):
        raw = patch_ota.parse(open(image, 'rb').read())[0][2]
        self.raw = raw
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.mem_map(0x10000000, 0x200000)
        uc.mem_write(BASE, raw)
        uc.mem_write(RET, b'\x00\xbf\x00\xbf')
        uc.mem_map(0x20000000, 0x40000)
        uc.mem_map(0x00100000, 0x20000)
        uc.mem_map(0x83000000, 0x100000)            # peripherals: harmless RAM
        uc.mem_map(0xE0000000, 0x100000)            # Cortex-M system control space
        d = 0x1014C4E4 - BASE
        uc.mem_write(0x20000000, raw[d:d + 0x1548])
        t = 0x1014DA2C - BASE
        uc.mem_write(0x00100000, raw[t:t + 0x2C18])
        try:
            uc.reg_write(A.UC_ARM_REG_CPACR, 0xF << 20)
        except Exception:
            pass
        self.heap = HEAP
        self.timers = []
        self.duty = {}
        self.calls = {}
        self.stub(0x1009c19a, self.malloc)                     # pvPortMalloc
        self.stub(0x100c9074, self.calloc)                     # pvPortCalloc
        self.stub(0x100c5324, lambda a: 0)                     # vPortFree (wrapper)
        self.stub(0x100c8fe4, lambda a: 0)                     # vPortFree (heap_4)
        self.stub(0x10107a34, lambda a: 0)                     # log_printf
        self.stub(0x100a8100, lambda a: 0)                     # arch_dbg_log_begin
        self.stub(0x100a8180, lambda a: 0)                     # arch_dbg_log_end
        self.stub(0x100c354c, self.timer_add)                  # yglc_timer_add(svc, timer)
        self.stub(0x100c358e, lambda a: int(a[0] in self.timers))   # yglc_timer_is_running
        self.stub(0x100c35a0, self.timer_del)                  # yglc_timer_del
        self.stub(0x1007355c, self.pwm_duty)                   # hal_pwm_set_duty_cycle
        self.stub(0x10073608, lambda a: 0)                     # hal_pwm_start
        self.stub(0x1007367c, self.pwm_running)                # hal_pwm_get_running_status
        self.stub(0x100c30b8, lambda a: 0)                     # report props to cloud/LAN
        self.stub(0x100bdbc0, lambda a: 0)                     # yglc_post_request_async
        self.stub(0x100c0158, lambda a: 0)                     # cron cancel by type
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.unmapped)

    # ---- stubs -------------------------------------------------------------------------
    def stub(self, addr, fn):
        def hook(uc, address, size, _):
            args = [uc.reg_read(r) for r in R]
            self.calls[addr] = self.calls.get(addr, 0) + 1
            uc.reg_write(A.UC_ARM_REG_R0, fn(args) & 0xffffffff)
            uc.reg_write(A.UC_ARM_REG_PC, uc.reg_read(A.UC_ARM_REG_LR))
        self.uc.hook_add(UC_HOOK_CODE, hook, begin=addr, end=addr)

    def malloc(self, a, zero=False):
        n = (a[0] + 7) & ~7
        p = self.heap
        self.heap += n + 8
        self.uc.mem_write(p, (b'\0' if zero else b'\xa5') * n)
        return p

    def calloc(self, a):
        return self.malloc([a[0] * a[1]], zero=True)

    def timer_add(self, a):
        if a[1] not in self.timers:
            self.timers.append(a[1])
        return 0

    def timer_del(self, a):
        if a[0] in self.timers:
            self.timers.remove(a[0])
        return 0

    def pwm_duty(self, a):
        self.duty[a[0] & 0xff] = a[1]
        return 0

    def pwm_running(self, a):
        self.uc.mem_write(a[1], b'\x01')
        return 0

    def unmapped(self, uc, access, address, size, value, _):
        pc = uc.reg_read(A.UC_ARM_REG_PC)
        print(f'UNMAPPED access {address:#x} at pc {pc:#x} lr {uc.reg_read(A.UC_ARM_REG_LR):#x}')
        return False

    # ---- helpers -----------------------------------------------------------------------
    def r32(self, a):
        return struct.unpack('<I', self.uc.mem_read(a, 4))[0]

    def call(self, fn, *args):
        uc = self.uc
        sp = SP_TOP - 0x100
        regs = list(args[:4])
        for i, v in enumerate(args[4:]):
            uc.mem_write(sp + 4 * i, struct.pack('<I', v & 0xffffffff))
        for r, v in zip(R, regs):
            uc.reg_write(r, v & 0xffffffff)
        uc.reg_write(A.UC_ARM_REG_SP, sp)
        uc.reg_write(A.UC_ARM_REG_LR, RET | 1)
        try:
            uc.emu_start(fn | 1, RET, count=20_000_000)
        except UcError as e:
            raise SystemExit(f'emulation error {e} in {fn:#x}: pc {uc.reg_read(A.UC_ARM_REG_PC):#x} '
                             f'lr {uc.reg_read(A.UC_ARM_REG_LR):#x}')
        if uc.reg_read(A.UC_ARM_REG_PC) != RET:
            raise SystemExit(f'{fn:#x} did not return (pc {uc.reg_read(A.UC_ARM_REG_PC):#x})')
        return uc.reg_read(A.UC_ARM_REG_R0)

    def run_timers(self, max_ticks=100000):
        """Fire due transition timers (cb @timer+0x1c, arg @timer+0x20) until idle."""
        ticks = 0
        trace = []
        while self.timers and ticks < max_ticks:
            batch = list(self.timers)
            self.timers.clear()
            for t in batch:
                cb, arg = self.r32(t + 0x1c), self.r32(t + 0x20)
                self.call(cb & ~1, t, arg)
            ticks += 1
            trace.append(self.state())
        if self.timers:
            raise SystemExit('transitions did not settle')
        return trace

    @property
    def unit(self):
        return self.r32(self.r32(0x20006908) + 0x58)

    def state(self):
        return (self.duty.get(CH_WARM, 0), self.duty.get(CH_COOL, 0), self.duty.get(CH_NL, 0))

    def props(self):
        u = self.unit
        dl = self.r32(u + 8)
        return {'off': self.r32(u), 'active': self.r32(u + 0x10), 'active_mode': self.r32(u + 0x13c),
                'bright': self.r32(dl + 0x24), 'ct': self.r32(dl)}


def expect_dl(b):
    """Expected warm-channel duty at 2700K for daylight bright b >= K."""
    bp = 1 + ((b - K) * 99 + (100 - K) // 2) // (100 - K)
    v = (bp / 100.0) * 4000.0
    f = FLOOR / 100.0
    return (f + (1 - f) * v / 4000.0) * 4000.0


def expect_nl(b):
    d = (K - 1) ** 2
    return min(100, max(1, (100 * b * b + d // 2) // d)) * 40


def main():
    e = Emu(IMAGE)
    e.call(0x100bad18)                                   # yglc_init_settings
    u = e.unit
    print(f'init OK, unit @ {u:#x}, heap used {e.heap - HEAP} bytes')
    fails = []

    def check(name, cond, info):
        print(f"  {'OK ' if cond else 'FAIL'} {name}: {info}")
        if not cond:
            fails.append(name)

    def settle(label):
        tr = e.run_timers()
        s = e.state()
        p = e.props()
        dark = sum(1 for w, c, n in tr if w == 0 and c == 0 and n == 0)
        print(f'{label:<34} pwm warm/cool/nl = {s}  {p}  ({len(tr)} ticks, {dark} fully dark)')
        return s, p, tr

    e.call(0x100c2574, 1, 0, 500)                        # boot: api_power_on(mask 1, mode 0)
    settle('boot power on')
    e.call(0x100c1e62, u, 2700, 300)                     # set_ct_keep_bright 2700
    settle('set_ct 2700')

    for b in [100, 50, 11, 10, 9, 5, 3, 1, 2, 9, 10, 40, 100, 1]:
        e.call(0x100c1d4a, u, b, 500, 0)                 # unit_set_bright
        (w, c, n), p, tr = settle(f'set_bright {b}')
        check(f'bright {b} app state', p['active_mode'] == 0 and p['bright'] == b and p['off'] == 0, p)
        if b >= K:
            check(f'bright {b} dl', abs(w - expect_dl(b)) <= 1 and c == 0 and n == 0,
                  f'warm {w} (expect {expect_dl(b):.0f}), cool {c}, nl {n}')
        else:
            check(f'bright {b} nl', w == 0 and c == 0 and n == expect_nl(b),
                  f'nl {n} (expect {expect_nl(b)}), warm {w}, cool {c}')
        dark = [i for i, (a1, a2, a3) in enumerate(tr) if a1 == 0 and a2 == 0 and a3 == 0]
        check(f'bright {b} never dark mid-way', not dark, f'dark ticks {dark[:5]}')

    e.call(0x100c1e62, u, 4000, 300)                     # CT change while in the hybrid
    (w, c, n), p, _ = settle('set_ct 4000 @ bright 1')
    check('ct change keeps hybrid', w == 0 and c == 0 and n == expect_nl(1) and p['active_mode'] == 0, p)
    e.call(0x100c1e62, u, 2700, 300)
    settle('set_ct 2700')

    e.call(0x100c1d4a, u, 3, 500, 0)
    settle('set_bright 3')
    e.call(0x100c1d12, u, 1, 500)                        # unit_set_power(off)
    (w, c, n), p, tr = settle('power off @ 3')
    check('power off dark', (w, c, n) == (0, 0, 0) and p['off'] == 1, p)
    check('power off: stock length (500 ms -> 50 steps)', len(tr) == 50, f'{len(tr)} steps')
    e.call(0x100c2574, 1, 0, 500)                        # power on again (wall switch path)
    (w, c, n), p, tr = settle('power on @ 3')
    check('power on returns to hybrid', (w, c, n) == (0, 0, expect_nl(3)) and p['bright'] == 3, p)
    check('power on fade unchanged (50 steps)', len(tr) == 50, f'{len(tr)} steps')
    e.call(0x100c1d4a, u, 60, 500, 0)
    settle('set_bright 60')
    e.call(0x100c1d12, u, 1, 500)
    (w, c, n), p, tr = settle('power off @ 60')
    warm = [x[0] for x in tr]
    print('    off @60 (stock), every 10th step  warm:', warm[::10], 'moon max', max(x[2] for x in tr))
    check('power off @ 60 = stock: 50 steps, main LEDs down to their minimum then 0, no moon',
          (w, c, n) == (0, 0, 0) and len(tr) == 50 and max(x[2] for x in tr) == 0
          and 380 <= warm[-2] <= 700 and p['off'] == 1, f'{len(tr)} steps, last lit {warm[-2]}')
    e.call(0x100c2574, 1, 0, 500)
    settle('power on @ 60')
    e.call(0x100c1d4a, u, 100, 500, 0)
    (w, c, n), p, tr = settle('set_bright 100 after power on')
    up = [tr[i][0] for i in range(0, 50, 10)]
    check('brightness change after power on uses the stock curve (slow start)',
          up[1] - up[0] < 0.05 * (4000 - up[0]), up)
    e.call(0x100c1d4a, u, 3, 500, 0)
    settle('set_bright 3')

    e.call(0x100c2050, u, 20, 500, 1)                    # real moonlight: enable_nl(20, set_status)
    (w, c, n), p, _ = settle('enable_nl 20 (moon mode)')
    check('moon mode untouched', (w, c, n) == (0, 0, 800) and p['active_mode'] == 1, p)
    e.call(0x100c1d4a, u, 50, 500, 0)                    # set_bright in moon mode -> nl_br
    (w, c, n), p, _ = settle('set_bright 50 in moon mode')
    check('moon set_bright', (w, c, n) == (0, 0, 2000) and p['active_mode'] == 1, p)
    e.call(0x100c20b4, u, 500, 1, 0)                     # disable_nl -> back to daylight @ 3
    (w, c, n), p, _ = settle('disable_nl -> day @ 3')
    check('back to hybrid day', (w, c, n) == (0, 0, expect_nl(3)) and p['active_mode'] == 0, p)
    e.call(0x100c1d4a, u, 60, 500, 0)
    (w, c, n), p, _ = settle('set_bright 60')
    check('day 60', abs(w - expect_dl(60)) <= 1 and n == 0, (w, c, n))

    # colour flow through the hybrid range: 2 steps, action 1 = stay at the last state
    flow = b'2,1,500,2,2700,5,500,2,2700,30\0'
    fp = e.malloc([len(flow)])
    e.uc.mem_write(fp, flow)
    e.call(0x100c232c, u, fp, 1)                        # unit_start_flow
    (w, c, n), p, tr = settle('flow 2700@5 -> 2700@30')
    peak = max(x[2] for x in tr)
    print(f'    flow: nl peak {peak}, dl at nl peak {[x[0] for x in tr if x[2] == peak][:1]}')
    saw_nl = peak >= 0.9 * expect_nl(5)
    check('flow passes through hybrid and ends on dl 30',
          saw_nl and abs(w - expect_dl(30)) <= 1 and n == 0, f'saw nl {expect_nl(5)}: {saw_nl}, end {(w, c, n)}')
    e.call(0x100c1d4a, u, 4, 500, 0)
    (w, c, n), p, _ = settle('set_bright 4 after flow')
    check('hybrid after flow', (w, c, n) == (0, 0, expect_nl(4)), (w, c, n))

    # ---- neutral CT: power off and on keeps the CT ----
    e.call(0x100c2574, 1, 0, 500)
    e.run_timers()
    e.call(0x100c1e62, u, 4500, 300)
    e.run_timers()
    e.call(0x100c1d4a, u, 60, 500, 0)
    e.run_timers()
    e.call(0x100c1d12, u, 1, 500)
    tr = e.run_timers()
    check('neutral CT power off = stock: 50 steps, dark, no moon',
          len(tr) == 50 and e.state() == (0, 0, 0) and max(x[2] for x in tr) == 0 and e.props()['off'] == 1,
          f'{len(tr)} steps')
    e.call(0x100c2574, 1, 0, 500)
    e.run_timers()
    check('neutral CT kept after power on', e.props()['ct'] == 4500 and e.state()[1] > 0, e.props())
    e.call(0x100c1e62, u, 2700, 300)
    e.run_timers()

    # ---- fail-safe latch: boot after a crash (g_warm_boot_type == 2) -> pure pass-through ----
    print('\n--- boot after a crash (g_warm_boot_type = 2) ---')
    e2 = Emu(IMAGE)
    e2.call(0x100bad18)
    e2.uc.mem_write(0x20008084, b'\x02')
    u2 = e2.unit
    e2.call(0x100c2574, 1, 0, 500)
    e2.run_timers()
    e2.call(0x100c1e62, u2, 2700, 300)
    e2.run_timers()
    for b in (50, 3, 1):
        e2.call(0x100c1d4a, u2, b, 500, 0)
        e2.run_timers()
        w, c, n = e2.state()
        exp = (FLOOR / 100 + (1 - FLOOR / 100) * b / 100) * 4000      # original path, floor 10 %
        check(f'latched bright {b} = original behaviour', abs(w - exp) <= 1 and n == 0,
              f'warm {w} (expect {exp:.0f}), nl {n}')
    e2.call(0x100c1d12, u2, 1, 500)
    tr = e2.run_timers()
    check('latched power off = stock (50 steps, stock curve)', len(tr) == 50 and e2.state() == (0, 0, 0),
          f'{len(tr)} steps')

    # ---- RAM crash latch of blackbox_monitor.c (bb_shared.h), boot type NOT 2 ----
    print('\n--- RAM crash latch (boot type 0) ---')
    BB, MAGIC, CRASHED, SEEN = 0x2003C000, 0x32424259, 0x53415243, 0x4e454553
    for latch, first_tick_done, expect_hybrid, what in (
            (CRASHED, 0, False, 'crash latch set by the fault handler, before the first monitor tick'),
            (SEEN, 1, False, 'latch converted by bb_boot of this boot'),
            (SEEN, 0, True, 'stale SEEN from an earlier boot, before the first monitor tick'),
            (0, 1, True, 'no latch')):
        e3 = Emu(IMAGE)
        e3.call(0x100bad18)
        e3.uc.mem_write(BB, struct.pack('<3I', MAGIC, MAGIC ^ 0xffffffff, latch))
        e3.uc.mem_write(0x20008034, struct.pack('<I', first_tick_done))
        u3 = e3.unit
        e3.call(0x100c2574, 1, 0, 500)
        e3.run_timers()
        e3.call(0x100c1e62, u3, 2700, 300)
        e3.run_timers()
        e3.call(0x100c1d4a, u3, 3, 500, 0)
        e3.run_timers()
        w, c, n = e3.state()
        hybrid_on = (w == 0 and n == expect_nl(3))
        check(f'{what}: hybrid {"on" if expect_hybrid else "off (pass-through)"}', hybrid_on == expect_hybrid,
              f'warm {w}, nl {n}')

    print(f"\n{'ALL CHECKS PASSED' if not fails else 'FAILED: ' + ', '.join(fails)}")
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
