"""Differential test: ORIGINAL firmware mixer functions (run in a Cortex-M4 emulator) vs
the clean C reconstruction firmware/clean_src/yglc_light_mixer_clean.c on the same grid.

Run: python tools/diff_light_mixer.py
Needs: unicorn (pip), gcc (w64devkit). Exit code 0 = identical output.
"""
import os
import shutil
import struct
import subprocess
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UcError
from unicorn import arm_const as A

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SEG = os.path.join(ROOT, 'extracted', 'segment-0-flash-00071000.bin')
BASE = 0x10071000
DATA_SRC, DATA_LEN = 0x1014C4E4, 0x1548
RET = 0x10000100           # magic return address inside mapped flash
M = 0x20030000             # mixer object
CAL = 0x20031000           # RAM copy of the dl CT table
NL = 0x20031400            # nl calib {5.0}
BUF = 0x20031800           # from/to/out arrays
SP = 0x2003f000

FN_CT_COMPUTE = 0x100ec71c
FN_SINGLE_COMPUTE = 0x100ec6a0
FN_EASE = 0x100ec2f0
FN_DL_INTERP = 0x100ec45c


class Fw:
    def __init__(self):
        self.seg = open(SEG, 'rb').read()
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.uc.mem_map(0x10000000, 0x200000)
        self.uc.mem_write(BASE, self.seg)
        self.uc.mem_map(0x20000000, 0x40000)
        d = DATA_SRC - BASE
        self.uc.mem_write(0x20000000, self.seg[d:d + DATA_LEN])
        self.uc.mem_write(RET, b'\x00\xbf\x00\xbf')
        # enable the FPU (CPACR CP10/CP11 full access)
        try:
            self.uc.reg_write(A.UC_ARM_REG_CPACR, 0xF << 20)
        except Exception:
            pass
        try:
            self.uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
        except Exception:
            pass
        self.cal = bytearray(self.seg[0x10140528 - BASE:0x10140528 - BASE + 0x15c])
        self.uc.mem_write(NL, struct.pack('<4f', 5.0, 0, 0, 0))

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))

    def r32(self, a):
        return struct.unpack('<I', self.uc.mem_read(a, 4))[0]

    def mixer(self, cal_ptr, floor_en, min_pct=0, curve=0, steps=0, tcurve=0):
        m = bytearray(100)
        struct.pack_into('<I', m, 0, 4000)
        m[4] = min_pct
        struct.pack_into('<I', m, 8, steps)
        m[0xc] = tcurve
        m[0xf] = floor_en
        struct.pack_into('<f', m, 0x14, 1.0)
        m[0x18] = curve
        struct.pack_into('<I', m, 0x1c, cal_ptr)
        self.uc.mem_write(M, bytes(m))

    def set_floor(self, f):
        struct.pack_into('<ff', self.cal, 0x154, f, f)
        self.uc.mem_write(CAL, bytes(self.cal))

    def call(self, fn, args, stack=()):
        sp = SP - 0x40
        for i, v in enumerate(stack):
            self.w32(sp + 4 * i, v)
        regs = (A.UC_ARM_REG_R0, A.UC_ARM_REG_R1, A.UC_ARM_REG_R2, A.UC_ARM_REG_R3)
        for r, v in zip(regs, args):
            self.uc.reg_write(r, v & 0xffffffff)
        self.uc.reg_write(A.UC_ARM_REG_SP, sp)
        self.uc.reg_write(A.UC_ARM_REG_LR, RET | 1)
        self.uc.emu_start(fn | 1, RET, count=200000)
        return self.uc.reg_read(A.UC_ARM_REG_R0)


def emulate():
    fw = Fw()
    out = []
    floors = [8.0, 6.5, 0.0, 10.5]
    mins = [0, 1, 15]
    for fi, fl in enumerate(floors):
        fw.set_floor(fl)
        for mi, mn in enumerate(mins):
            for cv in range(2):
                for ct in range(2600, 6601, 50):
                    b = 0
                    while b <= 101:
                        fw.mixer(CAL, 1, mn, cv)
                        fw.uc.mem_write(BUF, b'\0' * 16)
                        r = fw.call(FN_CT_COMPUTE, (M, ct, b, 0), (BUF,)) & 0xff
                        out.append(f'C {fi} {mi} {cv} {ct} {b} {r} {fw.r32(BUF)} {fw.r32(BUF + 4)}')
                        b += 1 if b < 20 else 7
    for mi, mn in enumerate(mins):
        for b in range(102):
            fw.mixer(NL, 0, mn, 0)
            fw.uc.mem_write(BUF, b'\0' * 16)
            r = fw.call(FN_SINGLE_COMPUTE, (M, 0, b, 0), (BUF,)) & 0xff
            out.append(f'S {mi} {b} {r} {fw.r32(BUF)}')
    pts = [(0, 4000), (4000, 0), (320, 1200), (1200, 320), (77, 78), (500, 500)]
    fw.set_floor(8.0)
    for cv in range(3):
        for p, (a, b) in enumerate(pts):
            for s in range(31):
                fw.mixer(CAL, 1, 0, 0, steps=30, tcurve=cv)
                r = fw.call(FN_EASE, (M, a, b, s), (a,))
                out.append(f'E {cv} {p} {s} {r}')
    pairs = [(0, 0, 2000, 1500), (2000, 1500, 0, 0), (0, 900, 900, 0), (900, 0, 0, 900),
             (320, 320, 4000, 4000), (0, 2000, 3000, 0), (3000, 0, 0, 2000), (400, 0, 1600, 0)]
    for fi, fl in enumerate(floors[:3]):
        fw.set_floor(fl)
        for p, (f0, f1, t0, t1) in enumerate(pairs):
            for s in range(22):
                fw.mixer(CAL, 1, 0, 0, steps=21)
                fw.uc.mem_write(BUF, struct.pack('<12I', f0, f1, 0, 0, t0, t1, 0, 0, f0, f1, 0, 0))
                fw.call(FN_DL_INTERP, (M, BUF, BUF + 16, s), (BUF + 32,))
                out.append(f'I {fi} {p} {s} {fw.r32(BUF + 32)} {fw.r32(BUF + 36)}')
    return out


def clean():
    gcc = shutil.which('gcc')
    if not gcc:
        raise SystemExit('gcc not found in PATH')
    exe = os.path.join(ROOT, 'firmware', 'build', 'test_light_mixer_diff.exe')
    subprocess.run([gcc, '-std=c11', '-O1', '-mfma', '-ffp-contract=off', '-Wall', '-Wextra', '-Werror',
                    '-DHOST_TEST=1', os.path.join(ROOT, 'firmware', 'tests', 'test_light_mixer_diff.c'),
                    os.path.join(ROOT, 'firmware', 'clean_src', 'yglc_light_mixer_clean.c'),
                    '-o', exe], check=True)
    return subprocess.run([exe, SEG], check=True, capture_output=True, text=True).stdout.split('\n')[:-1]


def main():
    try:
        orig = emulate()
    except UcError as e:
        raise SystemExit(f'emulation failed: {e}')
    mine = clean()
    if len(orig) != len(mine):
        raise SystemExit(f'line count differs: firmware {len(orig)} vs clean {len(mine)}')
    bad = [(o, c) for o, c in zip(orig, mine) if o != c]
    kinds = {}
    for o, _ in bad:
        kinds[o[0]] = kinds.get(o[0], 0) + 1
    print(f'{len(orig)} cases, {len(bad)} mismatches {kinds}')
    for o, c in bad[:25]:
        print(f'  firmware: {o}\n  clean   : {c}')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
