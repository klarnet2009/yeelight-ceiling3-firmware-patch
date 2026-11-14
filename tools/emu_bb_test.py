"""Emulator test of the network monitor v3 + black box (firmware/src/blackbox_monitor.c).

Runs the REAL segment 0 of the hybrid image (incl. the appended EXT code, the B.W at
0x100bd918 and the BL at 0x100ca1da) in a Cortex-M4 emulator. Network state, clocks, ARP
table, lwIP lock, psm and the reset functions are stubbed and driven by the test;
yglc_setting_entry_new and the property formatter are the real firmware code.

Run: python tools/emu_bb_test.py [image.bin]
"""
import os
import struct
import sys

sys.argv, _args = sys.argv[:1], sys.argv[1:]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emu_hybrid_test as T  # noqa: E402

IMAGE = _args[0] if _args else T.IMAGE
MON = 0x100bd918            # yglc_network_monitor (B.W to bb_monitor_tick)
DUMP_TAIL = 0x100ca1da      # register-dump function: bl bb_on_crash; pop {r3, r4, r5, pc}
BB = 0x2003C000
BB_TEXT_OFF = 0x3c
BB_LEN_OFF = 0x38
GW = 0x0100a8c0             # 192.168.0.1 (lwIP byte order)
NETIF = 0x20030800
FAULT = 0x20039a50
CFSR = 0xE000ED28
UTC0 = 0x69170000           # 14.11.2026
SYM = lambda n: T.patch_ota.hybrid_symbol(n, T.patch_ota.BB_ELF)


class Lamp(T.Emu):
    def __init__(self, image, psm, boot_type, noinit=None, clock=True):
        super().__init__(image)
        self.psm = psm
        self.net = 4
        self.now = 1000
        self.clock = clock
        self.lock_free = True
        self.lock_pattern = None
        self.lock_timeouts = set()
        self.probes = 0
        self.reboots = []
        self.resets = []
        self.psm_writes = []
        self.stub(0x100afc14, lambda a: self.net)                    # miio_net_get_state
        self.stub(0x100a85d4, lambda a: self.now)                    # arch_os_ms_now
        self.stub(0x100a8650, lambda a: self.utc())                  # UTC seconds
        self.stub(0x10083558, lambda a: NETIF)                       # netif st2
        self.stub(0x100c8620, self.take)                             # xQueueSemaphoreTake
        self.stub(0x100c8168, lambda a: 1)                           # xQueueGenericSend
        self.stub(0x1007d658, self.probe)                            # etharp_request
        self.stub(0x100f81e4, self.reboot)                           # miio_reboot
        self.stub(0x10104930, self.psm_get)                          # psm_get_variable
        self.stub(0x10104974, self.psm_set)                          # psm_set_variable
        self.stub(0x100a7fa8, lambda a: self.resets.append('arch_crash_reboot') or 0)
        self.stub(0x10072094, lambda a: self.resets.append(f'hal_wdt_init{tuple(a)}') or 0)
        self.stub(0x10072140, lambda a: self.resets.append('hal_wdt_software_reset') or 0)
        self.stub(0x10071e98, lambda a: self.resets.append(f'hal_sys_reboot({a[0]:#x}, {a[1]})') or 0)
        self.call(0x100bad18)                                        # yglc_init_settings
        self.uc.mem_write(0x20008084, bytes([boot_type]))           # g_warm_boot_type
        self.set_gateway(GW)
        self.uc.mem_write(0x20002c40, struct.pack('<I', 0x20030900))  # lock handle
        if noinit is not None:
            self.uc.mem_write(BB, noinit)

    # ---- stubs ----
    def utc(self):
        return (UTC0 if self.clock else 0) + self.now // 1000

    def take(self, a):
        self.lock_timeouts.add(a[1])
        if self.lock_pattern is not None:
            self.lock_pattern = not self.lock_pattern
            return 1 if self.lock_pattern else 0
        return 1 if self.lock_free else 0

    def probe(self, a):
        self.probes += 1
        return 0

    def reboot(self, a):
        self.reboots.append((self.now, self.cstr(a[1])))
        return 0

    def cstr(self, p, n=300):
        b = bytes(self.uc.mem_read(p, n))
        return b[:b.index(b'\0')].decode('latin1')

    def psm_get(self, a):
        key = self.cstr(a[1])
        if key not in self.psm:
            return 0xffffffff
        v = self.psm[key]
        v = (v if isinstance(v, bytes) else v.encode())[:a[3] - 1] + b'\0'
        self.uc.mem_write(a[2], v)
        return 0

    def psm_set(self, a):
        self.psm[self.cstr(a[1])] = self.cstr(a[2])
        self.psm_writes.append(self.now)
        return 0

    # ---- helpers ----
    def set_gateway(self, gw):
        self.uc.mem_write(NETIF + 0x0c, struct.pack('<I', gw))

    def arp(self, alive):
        ts = (self.now // 1000) - (5 if alive else 200)
        e = struct.pack('<IIIBBBBBBHB3xI', 0, GW, NETIF, 0, 0, 0, 0, 0, 0, 0, 2, ts & 0xffffffff)
        self.uc.mem_write(0x2000264c, e)

    def tick(self, seconds=0.5, arp=None):
        self.now += int(seconds * 1000)
        if arp is not None:
            self.arp(arp)
        self.call(MON)

    def run(self, minutes, step_s=5, arp=None):
        """Advance; stops at the first monitor reboot. Returns minutes until it (or None)."""
        t0 = self.now
        for _ in range(int(minutes * 60 / step_s)):
            self.tick(step_s, arp)
            if self.reboots:
                return (self.reboots[0][0] - t0) / 60000
        return None

    def log(self):
        return self.cstr(BB + BB_TEXT_OFF, 250)

    def toks(self):
        return [t for t in self.log().split(';') if t]

    def toks_nt(self):
        """Tokens without the per-boot T (clock) token."""
        return [t for t in self.toks() if not t.startswith('T')]

    def noinit(self):
        return bytes(self.uc.mem_read(BB, 0x200))

    def entry(self, name='bblog'):
        head = 0x2000805c
        node = self.r32(head)
        for _ in range(100):
            if not node or node == head:
                return None
            if self.cstr(self.r32(node + 0x24)) == name:
                return node
            node = self.r32(node)

    def prop(self):
        """What get_prop would return: the entry's own formatter (0x100c2b34) into a buffer."""
        e = self.entry()
        buf = 0x20031c00
        self.uc.mem_write(buf, b'\0' * 512)
        self.call(self.r32(e + 0x34) & ~1, e, buf)
        return self.cstr(buf, 512)

    def crash(self, pc, lr, cfsr):
        self.uc.mem_write(FAULT + 0x38, struct.pack('<II', lr, pc))
        self.uc.mem_write(CFSR, struct.pack('<I', cfsr))
        self.call(SYM('bb_on_crash'))


fails = []


def check(name, cond, info=''):
    print(f"  {'OK ' if cond else 'FAIL'} {name}: {info}")
    if not cond:
        fails.append(name)


def near(v, target, tol=0.3):
    return v is not None and target < v < target + tol


def main():
    psm = {}
    print('--- boot 1: power-on, empty flash, clock not set yet')
    L = Lamp(IMAGE, psm, 0, clock=False)
    L.run(0.9, 1)
    check('boot record + first network state', L.log() == 'B0;N4,0;', L.log())
    check('"bblog" registered; formatter returns the log', L.entry() is not None and L.prop() == L.log(), L.prop())
    check('no flash write in the first 55 s', not L.psm_writes, L.psm_writes)
    L.run(0.3, 1)
    check('first flash write ~60 s after boot', len(L.psm_writes) == 1 and psm['bblog'] == 'B0;N4,0;', psm)
    L.clock = True
    L.run(1)
    check('T token (boot UTC) logged once when the clock is known',
          L.toks()[-1] == f'T{UTC0:x}' and L.log().count('T') == 1, L.log())
    L.run(60)
    check('one more write for the T token, then silence', len(L.psm_writes) == 2, f'{len(L.psm_writes)} writes in 62 min')

    print('--- router off: series of monitor reboots with growing limits')
    L.net = 2
    dt = L.run(10, 1)
    check('1st reboot after ~5 min', near(dt, 5.0), f'{dt} min')
    last = L.toks()[-1]
    check('R token: reason D, series 1, UTC; flushed before the reboot',
          last.startswith('RD1,') and ',t69' in last and psm['bblog'].endswith(last + ';'), last)
    L2 = Lamp(IMAGE, psm, 1, L.noinit())
    L2.net = 2
    dt = L2.run(40)
    b = [t for t in L2.toks() if t.startswith('B')][-1]
    check('boot record after a monitor reboot: type 1, run length, last-alive UTC',
          b.startswith('B1,u') and ',t69' in b, b)
    check('2nd reboot only after ~30 min (RD2)', near(dt, 30.0) and L2.toks()[-1].startswith('RD2,'), f'{dt} min')
    L3 = Lamp(IMAGE, psm, 1, L2.noinit())
    L3.net = 2
    dt = L3.run(400, 10)
    check('3rd reboot only after ~6 h (RD3)', near(dt, 360.0, 0.5) and L3.toks()[-1].startswith('RD3,'), f'{dt} min')
    L4 = Lamp(IMAGE, psm, 1)                              # RAM block lost
    L4.net = 2
    dt = L4.run(400, 10)
    check('RAM lost: series restored from the flash text (still 6 h, RD4)',
          near(dt, 360.0, 0.5) and L4.toks()[-1].startswith('RD4,'), f'{dt} min')
    L5 = Lamp(IMAGE, psm, 0)                               # user power-cycles: RAM is gone
    L5.net = 2
    dt = L5.run(10, 1)
    check('power-on boot starts a new series (5 min, RD1)', near(dt, 5.0) and L5.toks()[-1].startswith('RD1,'), f'{dt} min')
    L6 = Lamp(IMAGE, psm, 1, L5.noinit())
    L6.net = 4
    L6.run(2)
    L6.net = 2
    dt = L6.run(10, 1)
    check('reaching CLOUD resets the series (5 min, RD1)', near(dt, 5.0) and L6.toks()[-1].startswith('RD1,'), f'{dt} min')

    print('--- between the reboot request and the reset (miio_reboot resets ~4 s later)')
    M = Lamp(IMAGE, {}, 0)
    M.run(1)
    M.net = 3
    dt = M.run(10, 5, arp=False)
    after_req = M.log()
    M.net = 2                                               # the reboot hooks stop Wi-Fi
    for _ in range(8):
        M.tick(0.5)
    check('nothing is logged after the reboot request (last token stays R)',
          near(dt, 5.0) and M.log() == after_req and M.toks()[-1].startswith('RA1,') and not M.resets, M.log()[-25:])
    for _ in range(60):
        M.tick(0.5)
    check('still running 30 s later: direct hal_sys_reboot as fallback',
          M.resets and set(M.resets) == {'hal_sys_reboot(0x20160626, 0)'}, M.resets[:2])
    M2 = Lamp(IMAGE, {}, 1, M.noinit())
    M2.net = 3
    dt = M2.run(40, 5, arp=False)
    check('series kept in RAM across that reboot: next limit 30 min (RA2)',
          near(dt, 30.0) and M2.toks()[-1].startswith('RA2,'), f'{dt} min')
    M3 = Lamp(IMAGE, {'bblog': 'B0;N4,0;RA1,5,t69170001;N2,5;'}, 1)     # RAM lost, N after R in flash
    M3.net = 2
    dt = M3.run(40)
    check('RAM lost, routine token after R in the flash log: series still found (30 min)',
          near(dt, 30.0) and M3.toks()[-1].startswith('RD2,'), f'{dt} min')

    print('--- OTA: no flash writes while updating')
    U = Lamp(IMAGE, {}, 0)
    U.run(2)
    w0 = len(U.psm_writes)
    U.net = 5
    U.run(30)
    check('UPDATING: the N5 token is not flushed during the update', len(U.psm_writes) == w0 and 'N5,' in U.log(),
          f'{len(U.psm_writes) - w0} writes')
    U.net = 4
    U.run(1)
    check('flushed once the update state is left', len(U.psm_writes) == w0 + 1, f'{len(U.psm_writes) - w0} writes')

    print('--- LOCAL: gateway checks')
    L7 = Lamp(IMAGE, psm, 0, L6.noinit())
    L7.run(1)
    L7.net = 3
    dt = L7.run(15, 5, arp=True)
    check('gateway alive: no reboot in 15 min, ARP probe every ~60 s', dt is None and 13 <= L7.probes <= 16, f'probes {L7.probes}')
    L7.set_gateway(0)
    dt = L7.run(20, 5, arp=False)
    check('no gateway configured: nothing to probe, no reboot', dt is None)
    L7.set_gateway(GW)
    dt = L7.run(10, 5, arp=False)
    check('gateway silent: reboot A after ~5 min', near(dt, 5.0) and L7.toks()[-1].startswith('RA1,'), f'{dt} min')

    print('--- lwIP core lock')
    L8 = Lamp(IMAGE, psm, 0, L7.noinit())
    L8.run(1)
    L8.lock_pattern = False                                 # busy on every other tick
    dt = L8.run(20, 0.5)
    check('CLOUD, lock busy every other tick: no reboot', dt is None)
    L8.lock_pattern = None
    L8.lock_free = False
    dt = L8.run(10, 1)
    check('CLOUD, lock stuck: reboot L after ~5 min', near(dt, 5.0) and L8.toks()[-1].startswith('RL1,'), f'{dt} min')
    L9 = Lamp(IMAGE, psm, 0, L8.noinit())
    L9.run(1)
    L9.net = 3
    L9.lock_free = False
    dt = L9.run(10, 1, arp=True)
    check('LOCAL, lock stuck: reboot L after ~5 min', near(dt, 5.0) and L9.toks()[-1].startswith('RL1,'), f'{dt} min')
    check('the lock is only ever taken with timeout 0', L7.lock_timeouts | L8.lock_timeouts | L9.lock_timeouts == {0},
          L7.lock_timeouts | L8.lock_timeouts | L9.lock_timeouts)

    print('--- provisioning / OTA: never reboot')
    L10 = Lamp(IMAGE, psm, 0, L9.noinit())
    L10.lock_free = False
    for st in (0, 1, 5):
        L10.net = st
        L10.run(20)
    check('UNPROV / UAP / UPDATING: no reboot in 60 min', not L10.reboots, L10.reboots)

    print('--- crash path')
    psm.clear()
    C1 = Lamp(IMAGE, psm, 0)
    C1.run(5)
    C1.crash(0x100c1d44, 0x100c1d3f, 0x00008200)
    check('crash hook: stock crash reboot, then the proven WDT reset as fallback',
          C1.resets == ['arch_crash_reboot', 'hal_wdt_init(0, 0, 0, 1)', 'hal_wdt_software_reset'], C1.resets)
    C1b = Lamp(IMAGE, psm, 0)
    C1b.run(1)
    C1b.uc.mem_write(FAULT + 0x38, struct.pack('<II', 0x1, 0x2))
    C1b.uc.reg_write(T.A.UC_ARM_REG_SP, T.SP_TOP - 0x200)
    C1b.uc.mem_write(T.SP_TOP - 0x200, struct.pack('<4I', 0, 0, 0, T.RET | 1))
    C1b.uc.emu_start(DUMP_TAIL | 1, T.RET, count=100000)
    check('patched BL in the register-dump function reaches the hook and returns',
          C1b.resets[:1] == ['arch_crash_reboot'] and C1b.r32(BB + 0x0c) == 2, C1b.resets)
    C2 = Lamp(IMAGE, psm, 2, C1.noinit())
    C2.run(1)
    b = [t for t in C2.toks() if t.startswith('B')][-1]
    check('boot after a crash logs PC / LR / CFSR / time', b.startswith('B2,u5,pc100c1d44,lr100c1d3f,cf8200,t69'), b)
    C2.run(3)
    C2.crash(0x100c0000, 0x100c0001, 0)
    C3 = Lamp(IMAGE, psm, 2, C2.noinit())
    C3.net = 2
    C3.run(20)
    check('2nd crash boot in a row (no 10 healthy min between): passive',
          not C3.reboots and C3.entry() is None and C3.log() == C2.log(), C3.log()[-40:])
    C4 = Lamp(IMAGE, psm, 1, C3.noinit())
    C4.run(15)
    C4.crash(0x100c0000, 0x100c0001, 0)
    C5 = Lamp(IMAGE, psm, 2, C4.noinit())
    C5.run(15)
    C5.crash(0x100c0000, 0x100c0001, 0)
    C6 = Lamp(IMAGE, psm, 2, C5.noinit())
    C6.run(1)
    check('crashes with 10+ healthy minutes between them: stays active',
          C6.entry() is not None and C6.toks_nt()[-2].startswith('B2,u15,'), C6.toks_nt()[-2:])
    C6.run(15)
    C6.crash(SYM('bb_monitor_tick') + 10, 0x100c0001, 0)
    ext_pc = SYM('bb_monitor_tick') + 10
    C7 = Lamp(IMAGE, psm, 2, C6.noinit())
    C7.run(1)
    check('crash inside this code: passive', C7.entry() is None)
    C8 = Lamp(IMAGE, psm, 0, C7.noinit())
    C8.run(0.5, 1)
    b = [t for t in C8.toks() if t.startswith('B')][-1]
    check('crash record kept through the passive boot and logged by the next active one',
          f'pc{ext_pc:x}' in b and b.startswith('B0,'), b)
    C8.crash(0x100c0000, 0x100c0001, 0)
    C9 = Lamp(IMAGE, psm, 2, C8.noinit())
    C9.run(1)
    check('crash within the first minute: passive', C9.entry() is None)
    C9b = Lamp(IMAGE, psm, 0, C8.noinit())                 # reset type was not saved to flash
    C9b.run(1)
    check('RAM crash latch alone (boot type 0) also counts as a crash boot: passive, latch = SEEN',
          C9b.entry() is None and C9b.r32(BB + 8) == 0x4e454553, hex(C9b.r32(BB + 8)))
    saved = {'bblog': 'RD1,5,t69170001;B2,u9,pc100c1d44,lr100c1d3f,cf8200,t69170002;N4,2;'}
    C10 = Lamp(IMAGE, saved, 2)
    C10.net = 2
    dt = C10.run(40)
    check('crash boot with the RAM block lost: passive, only "no network for 30 min -> reboot"',
          C10.entry() is None and near(dt, 30.0) and not C10.psm_writes, f'{dt} min')
    C11 = Lamp(IMAGE, saved, 1, C10.noinit())
    C11.run(2)
    check('the flash log survives that passive boot',
          saved['bblog'].startswith('RD1,5,t69170001;B2,u9,pc100c1d44,lr100c1d3f,cf8200,t69170002;N4,2;B1;'),
          saved['bblog'])

    print('--- corrupted data')
    good = C8.noinit()
    bad = bytearray(good)
    struct.pack_into('<I', bad, BB_LEN_OFF, 0x7fffffff)    # len garbage, magic intact
    D1 = Lamp(IMAGE, dict(psm), 1, bytes(bad))
    D1.run(1)
    check('RAM block with a bad length: rebuilt from flash, no fault', D1.toks_nt()[-2:] == ['B1', 'N4,0'], D1.log()[-30:])
    bad = bytearray(good)
    bad[BB_TEXT_OFF:BB_TEXT_OFF + 244] = b'\xff' * 244     # text without terminator
    D2 = Lamp(IMAGE, dict(psm), 1, bytes(bad))
    D2.run(1)
    check('RAM block with unterminated text: rebuilt, no fault', D2.toks_nt()[-2:] == ['B1', 'N4,0'], D2.log()[-30:])
    D3 = Lamp(IMAGE, {'bblog': b'B0;\x01\xfe garbage'}, 1)
    D3.run(1)
    check('unprintable text in flash: discarded', D3.log() == 'B1;N4,0;' + f'T{UTC0:x};', D3.log())

    print('--- window: incidents outlive routine tokens')
    E = Lamp(IMAGE, {'bblog': 'RD1,5,t69170001;B2,u9,pc100c1d44,lr100c1d3f,cf8200,t69170002;'}, 0)
    for i in range(200):
        E.net = 4 if i % 2 else 3
        E.tick(1, arp=True)
    lg = E.log()
    check('log <= 240 chars, well-formed tokens', len(lg) <= 240 and lg.endswith(';') and
          all(t[0] in 'BNRT' for t in E.toks()), f'{len(lg)} chars')
    check('incident records (R, B2) kept while N tokens rotate',
          lg.startswith('RD1,5,t69170001;B2,u9,pc100c1d44,lr100c1d3f,cf8200,t69170002;'), lg[:70])
    E2 = Lamp(IMAGE, {'bblog': ''.join(f'RD1,{i},t6917{i:04x};' for i in range(15))[:240].rsplit(';', 1)[0] + ';'}, 0)
    E2.run(1)
    check('log full of incidents: the oldest ones make room for new tokens',
          E2.log().endswith('B0;N4,0;' + f'T{UTC0:x};') and len(E2.log()) <= 240
          and not E2.log().startswith('RD1,0,'), E2.log()[:20])
    for i in range(200):
        E2.net = 4 if i % 2 else 3
        E2.tick(1, arp=True)
    inc = sum(len(t) + 1 for t in E2.toks() if t[0] == 'R' or t.startswith('B2'))
    check('incidents trimmed to <= 150 chars, the rest is recent routine history',
          inc <= 150 and len(E2.log()) > 200, f'incident chars {inc}, total {len(E2.log())}')

    print(f"\n{'ALL CHECKS PASSED' if not fails else 'FAILED: ' + ', '.join(fails)}")
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
